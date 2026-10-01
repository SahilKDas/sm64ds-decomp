// 16 DS-style mixer channels.
//
// The envelope lives in the DS's own domain: a s32 amplitude in units of
// 1/128 of a tenth of a decibel, running from -92544 (= -723 * 128, i.e.
// -72.3 dB, the DS's silence floor) up to 0. Attack is multiplicative in
// that log domain, decay and release are linear in it, and the rate
// conversion Cnv_Fall below is the ARM7's. The 0..127 -> decibel table is the
// ARM7's square-law one (sd_cnv_vol in sdat.cpp has the address and the
// check against the cartridge).
//
// Resampling is linear interpolation. The DS does the same thing in
// hardware, so this is not a shortcut.
//
// This file also owns the 192 Hz sequencer clock: sd_mix_render slices its
// output at 192 Hz boundaries and ticks the sequencer and every envelope
// there. Driving the sequencer off the AUDIO clock rather than the video
// frame is what the ARM7 does, and it keeps tempo correct even when the
// window's frame rate is not 60.
#include "sdat.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the voice trace ----------------------------------------------------
//
// SM64DS_VOICE_TRACE=1. Every allocation and every release of a voice, with
// the reason, at all three levels a sound effect travels through:
//
//   arm9   the 16 voice records func_0204fc40 builds, handed out by
//          func_0204f364 and returned by func_0204f2d4, plus the 3D
//          positional slots func_02048720 hands out. This is the level the
//          game itself budgets against, and the only one that can answer
//          "why did Sound::Play refuse". Printed by sd_vtrace_arm9_census in
//          consumer.cpp, which is where those globals are reachable.
//   play   sequencer players starting, stopping and finishing.
//   chan   the 16 mixer channels, and every note that never got one.
//
// The switch is latched once (sd_mix_reset runs before any sound can) and
// every call site is behind SD_VT, so with the variable off this costs one
// predictable branch per event and nothing else.
extern "C" int g_voice_trace;
int g_voice_trace;

void sd_vtrace(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[vt] ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

namespace {

enum EnvState { ENV_OFF = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

const sd_s32 AMPL_MIN = -723 * 128;     // -72.3 dB, in 1/128 of 0.1 dB

struct Channel {
    int active;
    const sd_s16 *pcm;
    sd_u32 total, loopStart;
    int loop;
    int hold;           // PSG voice: steps its samples, no interpolation
    double pos, step;

    sd_s32 ampl;
    int state;
    int attackCoef, decayRate, sustainLevel, releaseRate;

    int volDb10;        // combined external volume, tenths of a dB (<= 0)
    int pan;            // 0..127
    int priority;
    unsigned seq;       // bumped on every start, so stale handles are inert

    /* THE CHANNEL'S OWN PITCH, as SM64DS's ARM7 composes it every frame in its
       channel update (0x037FC5F4..0x037FC668, called once per 192 Hz frame for
       every active channel whether or not a track still owns it):

           pitch = (key - root) * 64 + sweep + userPitch + lfo

       baseStep is the (key - root) term as a playback rate, userPitch the
       track's bend + ext pitch (ch+0x0E, written by the track update while the
       track owns the channel), and the sweep is the channel's own countdown
       (ch+0x32 sweepPitch, +0x18 sweepLength, +0x14 sweepCounter; 0x037FBC34):
       sweepPitch * (length - counter) / length while counter < length, the
       counter stepping once per frame. It is what portamento and the 0xE3
       sweep play through, and it keeps moving after the track lets go of the
       channel, which is why it lives here and not in the sequencer.

       pitchUnits is the total last turned into `step`, so a voice whose pitch
       did not move this frame costs no pow(). hasBase is 0 for a channel the
       sequencer never described (nothing then retunes it). */
    int hasBase;
    double baseStep;
    int userPitch;
    int sweepPitch, sweepLength, sweepCounter;
    int modPitch;       // the sweep (and modulation) share of pitchUnits
    int pitchUnits;

    /* THE CHANNEL'S LFO (ch+0x28: target, speed, depth, range, u16 delay;
       +0x2e delay counter, +0x30 phase counter), copied from the owning
       track's modulation fields every frame by the track update
       (0x037FD710..0x037FD724) and run by the channel update: value
       0x037FBEEC, step 0x037FBF3C, scaling 0x037FBB98. Target 0 bends the
       pitch, 1 the volume, 2 the pan; depth 0 is off, which is every voice
       that never asked for modulation. lfoDb10 / lfoPan are this frame's
       volume and pan terms, added at the render. */
    int lfoTarget, lfoSpeed, lfoDepth, lfoRange, lfoDelay;
    int lfoDelayCounter, lfoCounter;
    int lfoDb10, lfoPan;
};

Channel g_ch[SD_CHANNELS];

// Frames of 32768 Hz output per 192 Hz sequencer tick, as a fraction.
int g_tickAcc;          // counts output frames * 192

int cnv_attack(int a)
{
    static const sd_u8 lut[19] = {
        0, 1, 5, 14, 26, 38, 51, 63, 73, 84, 92, 100, 109, 116, 123, 127,
        132, 137, 143
    };
    if (a >= 0x6d) return lut[0x7f - a];
    return 255 - a;
}

int cnv_fall(int f)
{
    if (f == 0x7f) return 0xffff;       // instant
    if (f == 0x7e) return 0x3c00;
    if (f < 0x32) return f * 2 + 1;
    return 0x1e00 / (0x7e - f);
}

// Lane VOICE: the post-master-volume render hook. See sd_mix_render's tail.
void (*g_aux_render)(sd_s16 *, int);

double db10_to_gain(int db10)
{
    if (db10 <= -723) return 0.0;
    if (db10 > 0) db10 = 0;
    return pow(10.0, db10 / 200.0);
}

}  // namespace

namespace { void ct_open(void); }   // SM64DS_CHAN_TRACE, below sd_mix_frame

// port/rollback: the output stage muted while a rewound window is re-run.
// Voices still start and envelopes still advance, so a sound that began
// inside the window is heard from where the replay leaves it; only the bytes
// on the way to the device are zeroed. One int, read on the render path.
static volatile int g_host_mute = 0;
extern "C" void sd_host_mute(int on) { g_host_mute = on ? 1 : 0; }

void sd_mix_reset(void)
{
    static int latched;
    if (!latched) {
        latched = 1;
        g_voice_trace = getenv("SM64DS_VOICE_TRACE") != 0;
        ct_open();
    }
    memset(g_ch, 0, sizeof g_ch);
    g_tickAcc = 0;
}

/* THE ROM'S OWN CHANNEL ALLOCATOR, transcribed from the ARM7 driver.
 *
 * The routine is at 0x037FC26C in the ARM7 image (arm7.bin offset 0x043D4;
 * the driver is autoloaded to 0x037F8000, so runtime = fileoff - 0x168 +
 * 0x037F8000). Its 16-channel table is at 0x038075C4, stride 0x54, with the
 * priority a u8 at +0x22 and the volume pair a u16 at +0x24.
 *
 * WHAT IT ACTUALLY DOES, and where this port used to differ:
 *
 *   1. IT SCANS IN A FIXED ORDER THAT IS NOT 0..15. The loop indexes a
 *      16-byte table at 0x03805964 holding
 *          4, 5, 6, 7, 2, 0, 3, 1, 8, 9, 10, 11, 14, 12, 15, 13
 *      Nothing in the ARM7 image ever writes that table. It decides which
 *      channel wins a complete priority-and-volume tie, so scanning 0..15
 *      instead picks a different victim.
 *
 *   2. THERE IS NO "IS THIS CHANNEL BUSY" TEST. This port used to hand back
 *      the first inactive channel outright. The ROM has no such branch: a
 *      channel that has STOPPED is given priority 0 by the per-frame update
 *      (0x037FC5A8, 0x037FC6A4 and 0x037FC7AC all store 0 to +0x22), so it
 *      sorts to the front on its own and needs no special case. Matching that
 *      is why sd_mix_kill and the two envelope-end paths below now zero the
 *      priority.
 *
 *   3. STRICTLY LOWER PRIORITY WINS, then QUIETEST WINS. Equal priority is
 *      broken by the compare at 0x037FBD54, which returns -1 when the
 *      incumbent is LOUDER and the caller then takes the challenger. It is a
 *      STRICT less-than, so an exact tie keeps whichever came first in scan
 *      order. This port had no volume tie-break at all: it kept the lowest
 *      channel INDEX, which is what made channel 0 absorb 274 of the opening's
 *      1157 note starts while channel 15 took 37.
 *
 *   4. A CHANNEL IN ITS RELEASE TAIL KEEPS ITS FULL PRIORITY. It gets no
 *      discount; only a channel that has reached the floor drops to 0. So the
 *      release tails this mixer holds are faithful, and the thing that
 *      resolves them is (3): among equals the fading one is the quietest and
 *      is therefore the one taken.
 *
 *   5. ACCEPT IFF requested >= best. The final test is a single
 *      "cmp P, B / blt return NULL" at 0x037FC328, so an EQUAL priority may
 *      steal. This port already agreed here.
 *
 * The one place the port cannot be bit-exact: the ROM compares
 * (vol & 0xFF) << 4 >> shiftTable[vol >> 8], shiftTable = {0,1,2,4}, i.e. a
 * 0..127 volume paired with a hardware divider. This mixer collapses both into
 * a single logarithmic attenuation, so the ORDERING is identical but the
 * quantisation is finer: two channels the ROM would call exactly equal can be
 * ordered here. That can only change which of two equally loud, equal priority
 * channels is taken, never whether a note is dropped.
 */
namespace {

const int kScanOrder[SD_CHANNELS] = {
    4, 5, 6, 7, 2, 0, 3, 1, 8, 9, 10, 11, 14, 12, 15, 13
};

/* The tie-break quantity, in tenths of a decibel of attenuation (always <= 0;
   quieter is smaller). c.ampl is the envelope in 1/128 of a tenth of a dB and
   c.volDb10 is the already-combined external volume. */
int chan_attenuation_db10(const Channel &c)
{
    return c.volDb10 + (int)(c.ampl / 128);
}

}  // namespace

int sd_mix_alloc(int priority)
{
    return sd_mix_alloc_mask(priority, 0xffffu);
}

/* THE CHANNEL SET A NOTE MAY TAKE. The allocator's first argument at
 * 0x037FC26C is the player's channel mask AND the note type's set, chosen at
 * the note-on (0x037FD49C): a sampled note any of the 16 (0xFFFF), a PSG
 * pulse note channels 8..13 (0x3F00), a PSG noise note channels 14 and 15
 * (0xC000). The scan below is the same one, skipping the channels outside the
 * set; a mask of 0xFFFF is the plain allocator. */
int sd_mix_alloc_mask(int priority, unsigned mask)
{
    int best = -1;
    for (int k = 0; k < SD_CHANNELS; k++) {
        const int i = kScanOrder[k];
        if (!(mask & (1u << i))) continue;
        if (best < 0) { best = i; continue; }   /* first candidate, outright */
        if (g_ch[i].priority > g_ch[best].priority) continue;
        if (g_ch[i].priority == g_ch[best].priority
            && chan_attenuation_db10(g_ch[i])
                   >= chan_attenuation_db10(g_ch[best]))
            continue;                           /* strict: a tie keeps the first */
        best = i;
    }
    if (best < 0) return -1;                    /* unreachable: 16 candidates */

    if (priority < g_ch[best].priority) {
        SD_VT("chan ALLOC FAILED: quietest lowest-priority channel is %d at "
              "priority %d, above the requested %d\n", best,
              g_ch[best].priority, priority);
        if (g_voice_trace) {
            /* "all 16 sounding" is not the same claim as "all 16 audible": a
               released channel still holds its slot, and its full priority,
               until its envelope reaches the floor. Printing the state next to
               the priority is what tells a genuinely busy scene apart from a
               pile of release tails. */
            static const char *st[] = { "off", "atk", "dec", "sus", "rel" };
            for (int i = 0; i < SD_CHANNELS; i++)
                sd_vtrace("    chan %2d: prio %3d, %s, atten %d dB10%s\n", i,
                          g_ch[i].priority,
                          st[g_ch[i].state >= 0 && g_ch[i].state <= 4
                             ? g_ch[i].state : 0],
                          chan_attenuation_db10(g_ch[i]),
                          g_ch[i].active ? "" : ", INACTIVE");
        }
        return -1;
    }
    if (g_ch[best].active) {
        SD_VT("chan %2d STOLEN for priority %d (victim priority %d, atten "
              "%d dB10)\n", best, priority, g_ch[best].priority,
              chan_attenuation_db10(g_ch[best]));
        sd_pd_end(best, g_ch[best].priority, "stolen");
    }
    g_ch[best].active = 0;
    return best;
}

void sd_mix_start(int ch, const SdatWave *w, const SdatNote *n,
                  int volume_db10, int pan, double rate, int priority)
{
    if (ch < 0 || ch >= SD_CHANNELS || !w || !w->pcm || !w->totalSamples) {
        SD_VT("chan %2d start REFUSED: empty wave\n", ch);
        return;
    }
    // The rate is the field an ear notices and a count cannot: it is sample
    // frames consumed per output frame, so 1.0 plays the wave at the pitch
    // it was recorded at and 0.25 is two octaves down AND four times as
    // long. "Pitched down and stretched and dragging on" is this one number
    // coming out small, and it belongs next to the length it multiplies.
    SD_VT("chan %2d start: %u samples%s, %d dB10, pan %d, prio %d, "
          "rate %.4f (%u Hz wave, %.0f ms)\n", ch,
          (unsigned)w->totalSamples, w->loop ? " looping" : "", volume_db10,
          pan, priority, rate, (unsigned)w->sampleRate,
          rate > 0.0 ? (double)w->totalSamples * 1000.0 / (rate * SD_MIX_RATE)
                     : 0.0);
    Channel &c = g_ch[ch];
    unsigned s = c.seq + 1;
    memset(&c, 0, sizeof c);
    c.seq = s;
    c.active = 1;
    c.pcm = w->pcm;
    c.total = w->totalSamples;
    c.loop = w->loop;
    c.loopStart = w->loopStart;
    c.pos = 0.0;
    c.step = rate;
    c.volDb10 = volume_db10;
    c.pan = pan < 0 ? 0 : (pan > 127 ? 127 : pan);
    c.priority = priority;
    /* The LFO defaults the channel setup gives every new voice (0x037FBFBC:
       target pitch, speed 16, depth 0, range 1, delay 0) and the counters the
       start clears (0x037FBFAC). */
    c.lfoSpeed = 16;
    c.lfoRange = 1;

    c.ampl = AMPL_MIN;
    c.state = ENV_ATTACK;
    c.attackCoef  = cnv_attack(n ? n->attack : 127);
    c.decayRate   = cnv_fall(n ? n->decay : 127);
    c.releaseRate = cnv_fall(n ? n->release : 127);
    c.sustainLevel = sd_cnv_vol(n ? n->sustain : 127) * 128;
}

/* A PSG voice (pulse or noise) is stepped, not interpolated: the hardware
   channel holds one sample per timer period and the pulse's edges stay edges. */
void sd_mix_set_hold(int ch)
{
    if (ch >= 0 && ch < SD_CHANNELS) g_ch[ch].hold = 1;
}

void sd_mix_set(int ch, int volume_db10, int pan, double rate)
{
    if (ch < 0 || ch >= SD_CHANNELS || !g_ch[ch].active) return;
    g_ch[ch].volDb10 = volume_db10;
    g_ch[ch].pan = pan < 0 ? 0 : (pan > 127 ? 127 : pan);
    if (rate > 0.0) g_ch[ch].step = rate;
}

void sd_mix_set_pan(int ch, int pan)
{
    if (ch < 0 || ch >= SD_CHANNELS || !g_ch[ch].active) return;
    g_ch[ch].pan = pan < 0 ? 0 : (pan > 127 ? 127 : pan);
}

void sd_mix_set_vol(int ch, int volume_db10)
{
    if (ch < 0 || ch >= SD_CHANNELS || !g_ch[ch].active) return;
    g_ch[ch].volDb10 = volume_db10 < -723 ? -723
                     : (volume_db10 > 0 ? 0 : volume_db10);
}

// Retune a voice already sounding. TRACK_PARAM 0x0c arrives every frame for a
// moving sound, so the pitch of a voice has to be changeable without touching
// its level or its pan -- sd_mix_set above writes all three, and the two the
// caller did not mean to change would ride along.
void sd_mix_set_rate(int ch, double rate)
{
    if (ch < 0 || ch >= SD_CHANNELS || !g_ch[ch].active) return;
    if (rate > 0.0) g_ch[ch].step = rate;
}

namespace {

/* 1/64 of a semitone as a playback-rate multiplier. The same expression the
   sequencer has always used (sseq.cpp pitch_units_scale), so a voice whose
   pitch terms are all zero apart from the track's comes out at the rate it
   always did, to the last bit. */
double units_scale(int units)
{
    return pow(2.0, (double)units / 64.0 / 12.0);
}

/* Turn the channel's total pitch into its playback rate, if it moved. The
   same refusal the sequencer applies at the note-on: a rate outside (0, 64]
   is left where it was rather than stopping a voice that is sounding. */
void apply_pitch(Channel &c)
{
    if (!c.hasBase) return;
    const int units = c.userPitch + c.modPitch;
    if (units == c.pitchUnits) return;
    c.pitchUnits = units;
    const double rate = c.baseStep * units_scale(units);
    if (rate > 0.0 && rate <= 64.0) c.step = rate;
}

}  // namespace

/* The note-on half of the channel's pitch: its (key - root) rate and the
   track pitch it starts under. The caller has already started the channel at
   baseStep * scale(userPitch), so this records the parts without moving it. */
void sd_mix_set_pitch_base(int ch, double baseStep, int userPitch)
{
    if (ch < 0 || ch >= SD_CHANNELS || !g_ch[ch].active) return;
    Channel &c = g_ch[ch];
    c.hasBase = 1;
    c.baseStep = baseStep;
    c.userPitch = userPitch;
    c.modPitch = 0;             // the rate the caller set carries no sweep yet
    c.pitchUnits = userPitch;
}

/* ch+0x0E, the track's bend + ext pitch. Applied at once, as the port's
   TRACK_PARAM 0x0c path always has been. */
void sd_mix_set_user_pitch(int ch, int units)
{
    if (ch < 0 || ch >= SD_CHANNELS || !g_ch[ch].active) return;
    g_ch[ch].userPitch = units;
    apply_pitch(g_ch[ch]);
}

/* The sweep a note starts with (the note-on at 0x037FD5B8..0x037FD61C):
   sweepPitch in 1/64 semitones, sweepLength in 192 Hz frames, the counter
   back to 0. The first frame plays the whole offset. */
void sd_mix_set_sweep(int ch, int sweepPitch, int sweepLength)
{
    if (ch < 0 || ch >= SD_CHANNELS || !g_ch[ch].active) return;
    Channel &c = g_ch[ch];
    c.sweepPitch = sweepPitch;
    c.sweepLength = sweepLength;
    c.sweepCounter = 0;
}

/* The track's envelope override (-1 = keep the instrument's), converted the
   way the ARM7's setters convert it: attack 0x037FC3FC (255 - a below 0x6d,
   the 19-entry table above), decay 0x037FC3E0 and release 0x037FC3BC
   (0x037FBDB4, cnv_fall here), sustain 0x037FC3D8 stored raw and turned into
   a level where the decay reads it. The sustain level uses the same
   conversion sd_mix_start gives the instrument's own sustain. */
void sd_mix_set_env(int ch, int attack, int decay, int sustain, int release)
{
    if (ch < 0 || ch >= SD_CHANNELS || !g_ch[ch].active) return;
    Channel &c = g_ch[ch];
    if (attack >= 0) c.attackCoef = cnv_attack(attack);
    if (decay >= 0) c.decayRate = cnv_fall(decay);
    if (sustain >= 0) c.sustainLevel = sd_cnv_vol(sustain) * 128;
    if (release >= 0) c.releaseRate = cnv_fall(release);
}

/* The track's modulation fields, as the track update copies them onto every
   channel it owns each frame. The counters are the channel's and are left
   alone. */
void sd_mix_set_lfo(int ch, int target, int speed, int depth, int range,
                    int delay)
{
    if (ch < 0 || ch >= SD_CHANNELS || !g_ch[ch].active) return;
    Channel &c = g_ch[ch];
    c.lfoTarget = target & 0xff;
    c.lfoSpeed = speed & 0xff;
    c.lfoDepth = depth & 0xff;
    c.lfoRange = range & 0xff;
    c.lfoDelay = delay & 0xffff;
}

namespace {

/* The ARM7's LFO sine, a 33-entry quarter wave: round(127 * sin(pi/2 * i/32)).
   The formula reproduces the cartridge's table (arm7.bin, just below the
   decibel table at 0x03805860) entry for entry. 0x037FB6F4 folds the 0..0x7f
   phase onto it. */
int lfo_sin(int x)
{
    static signed char t[33];
    static int built;
    if (!built) {
        built = 1;
        for (int i = 0; i <= 32; i++)
            t[i] = (signed char)floor(127.0 * sin(1.5707963267948966 * i / 32.0)
                                      + 0.5);
    }
    if (x < 0x20) return t[x];
    if (x < 0x40) return t[0x40 - x];
    if (x < 0x60) return -t[x - 0x40];
    return -t[0x20 - (x - 0x60)];
}

/* One frame of the channel's LFO: the value at the current phase, THEN the
   step (0x037FBB98 reads before it advances). Returns the raw
   sin * depth * range product; the caller scales it by target. */
int lfo_frame(Channel &c)
{
    int v = 0;
    if (c.lfoDepth != 0 && c.lfoDelayCounter >= c.lfoDelay)
        v = lfo_sin(c.lfoCounter >> 8) * c.lfoDepth * c.lfoRange;
    if (c.lfoDelayCounter < c.lfoDelay) {
        c.lfoDelayCounter++;
    } else {
        const int inc = c.lfoSpeed << 6;
        int hi = (c.lfoCounter + inc) >> 8;
        while (hi >= 0x80) hi -= 0x80;
        c.lfoCounter = ((c.lfoCounter + inc) & 0xff) | (hi << 8);
    }
    return v;
}

}  // namespace

/* The release rate this channel fades at, in the envelope's own units per
   192 Hz tick, and how many ticks it needs to reach the -72.3 dB floor from
   where it is now. A channel holds its slot and its full allocation priority
   for every one of those ticks -- the ARM7 only zeroes the priority once the
   floor is reached -- so this number is the difference between a tail and a
   leak. At rate 1 the fade is 92544 ticks, which is 482 seconds. */
int sd_mix_release_rate(int ch)
{
    if (ch < 0 || ch >= SD_CHANNELS) return 0;
    return g_ch[ch].releaseRate;
}

int sd_mix_release_ticks(int ch)
{
    if (ch < 0 || ch >= SD_CHANNELS) return 0;
    const int r = g_ch[ch].releaseRate;
    if (r <= 0) return -1;                  /* never gets there */
    const sd_s32 span = g_ch[ch].ampl - AMPL_MIN;
    if (span <= 0) return 0;
    return (int)(span / r) + 1;
}

void sd_mix_release(int ch, const char *why)
{
    if (ch < 0 || ch >= SD_CHANNELS || !g_ch[ch].active) return;
    SD_VT("chan %2d release: %s@", ch, why);
    g_ch[ch].state = ENV_RELEASE;
    /* A RELEASED CHANNEL COLLAPSES TO PRIORITY 1. This is the ARM7's, not a
       heuristic: ReleaseTrackChannels at 0x037FD948 does

           strb r4, [r6, #0x22]   ; ch->priority = 1   (r4 = 1)
           bl   0x037fc3b0        ; ch->envState  = 3  (RELEASE)

       and the note-length expiry path reaches the same state through the steal
       callback at 0x037FD744, whose reason-1 arm is "mov r1,#1 / strb r1,
       [r5,#0x22]". Priority 0 is NOT the same thing and is not interchangeable:
       the ROM reserves 0 for a channel that has been fully reclaimed, and uses
       1 for one that is still audibly fading.

       WHY IT MATTERS HERE. Releasing without dropping the priority is what made
       the opening's music disappear for six seconds. A sound-effect voice is
       allocated at cpr 96 + track 64 = 160. When its track ends, this port put
       it in ENV_RELEASE but left it sitting at 160, so it went on outranking
       the music -- which asks at 129 to 131 -- for the entire length of its
       fade, and with a slow release rate that is many seconds. Sixteen of those
       held every channel at once. On hardware the same voice is priority 1 the
       moment its track ends: still sounding, but the cheapest thing in the
       room, so the music takes the channel straight back.

       The old allocator hid this. It had no volume tie-break and would steal
       an equal-priority channel by index, so the music got channels back by
       accident. Making the allocator faithful removed the accident and left
       the real defect exposed. */
    g_ch[ch].priority = 1;
}

void sd_mix_kill(int ch, const char *why)
{
    if (ch < 0 || ch >= SD_CHANNELS) return;
    if (g_ch[ch].active) SD_VT("chan %2d kill: %s\n", ch, why);
    sd_pd_end(ch, g_ch[ch].priority, "kill");
    g_ch[ch].active = 0;
    g_ch[ch].state = ENV_OFF;
    g_ch[ch].priority = 0;   /* stopped sorts first, as on the ARM7 */
}

int sd_mix_active(int ch)
{
    return (ch >= 0 && ch < SD_CHANNELS) ? g_ch[ch].active : 0;
}

/* ---- SM64DS_CHAN_TRACE=<path> (or =1 for stderr): the per-frame channel trace --
 *
 * One line per sounding channel per 192 Hz frame IN WHICH ITS OUTPUT MOVED:
 * the playback rate, the total attenuation the render applies (envelope plus
 * external volume, tenths of a dB, floored at -723) and the pan. It is the
 * port's side of the question "does this voice bend / fade / move the way the
 * sequence data says", answered per frame rather than per note-on, which is
 * the one thing SM64DS_PITCH_DUMP cannot show: a voice whose pitch never moves
 * prints ONE line there whether or not the data asked it to move.
 *
 * Latched once, off by default, one predictable branch per frame when off. It
 * reads the channel state and writes nothing back. */
namespace {
FILE *g_ctFile;
int g_ctOn;
unsigned g_ctFrame;
struct CtLast { double rate; int vol, pan, state, on; };
CtLast g_ctLast[SD_CHANNELS];

void ct_open(void)
{
    static int latched;
    if (latched) return;
    latched = 1;
    const char *p = getenv("SM64DS_CHAN_TRACE");
    if (!p || !*p) return;
    g_ctFile = (!strcmp(p, "1") || !strcmp(p, "-")) ? stderr : fopen(p, "w");
    if (!g_ctFile) return;
    g_ctOn = 1;
    fprintf(g_ctFile, "[ct] f = 192 Hz frame; rate = sample frames per output "
            "frame at %d Hz; vol = envelope + external volume in tenths of a "
            "dB (-723 = silent); pan 0..127\n", (int)SD_MIX_RATE);
}

void ct_frame(void)
{
    g_ctFrame++;
    static const char *st[] = { "off", "atk", "dec", "sus", "rel" };
    for (int i = 0; i < SD_CHANNELS; i++) {
        const Channel &c = g_ch[i];
        CtLast &l = g_ctLast[i];
        if (!c.active) {
            if (l.on) fprintf(g_ctFile, "[ct] f=%u ch=%d end\n", g_ctFrame, i);
            l.on = 0;
            continue;
        }
        int vol = (c.ampl >> 7) + c.volDb10 + c.lfoDb10;
        if (vol < -723) vol = -723;
        int pan = c.pan + c.lfoPan;
        pan = pan < 0 ? 0 : (pan > 127 ? 127 : pan);
        if (l.on && l.rate == c.step && l.vol == vol && l.pan == pan
            && l.state == c.state && c.seq == (unsigned)l.on)
            continue;
        fprintf(g_ctFile, "[ct] f=%u ch=%d %s rate=%.6f vol=%d pan=%d%s\n",
                g_ctFrame, i, st[c.state >= 0 && c.state <= 4 ? c.state : 0],
                c.step, vol, pan,
                (!l.on || c.seq != (unsigned)l.on) ? " start" : "");
        l.rate = c.step; l.vol = vol; l.pan = pan; l.state = c.state;
        l.on = (int)c.seq;
    }
}
}  // namespace

void sd_mix_frame(void)
{
    for (int i = 0; i < SD_CHANNELS; i++) {
        Channel &c = g_ch[i];
        if (!c.active) continue;
        /* The sweep term, 0x037FBC34: read at the current counter, then the
           counter steps. The division is the ROM's 64-bit signed one. */
        int sweep = 0;
        if (c.sweepPitch != 0 && c.sweepCounter < c.sweepLength) {
            sweep = (int)((long long)c.sweepPitch
                          * (c.sweepLength - c.sweepCounter) / c.sweepLength);
            c.sweepCounter++;
        }
        /* The LFO term, scaled by its target the ARM7's way (0x037FBBC4..:
           pitch and pan << 6, volume * 60, then >> 14 on the 64-bit value)
           and routed where the channel update routes it (0x037FC630..). */
        int lfoPitch = 0;
        c.lfoDb10 = 0;
        c.lfoPan = 0;
        const int lv = lfo_frame(c);
        if (lv != 0) {
            switch (c.lfoTarget) {
            case 0: lfoPitch = (int)(((long long)lv * 64) >> 14); break;
            case 1: c.lfoDb10 = (int)(((long long)lv * 60) >> 14); break;
            case 2: c.lfoPan = (int)(((long long)lv * 64) >> 14); break;
            default: break;
            }
        }
        c.modPitch = sweep + lfoPitch;
        apply_pitch(c);
        switch (c.state) {
        case ENV_ATTACK:
            /* ampl is negative and climbs toward 0 multiplicatively, the
               ARM7's way (0x037FBCC0..0x037FBCE8): -((-ampl * coef) >> 8),
               decay once it reads exactly 0. This used to divide by 255,
               which is a different curve -- slower for every attack
               coefficient, and never reaching 0 at all for coefficient 255
               (attack 0). */
            c.ampl = -(sd_s32)(((long long)(-c.ampl) * c.attackCoef) >> 8);
            if (c.ampl >= 0) { c.ampl = 0; c.state = ENV_DECAY; }
            break;
        case ENV_DECAY:
            c.ampl -= c.decayRate;
            if (c.ampl <= c.sustainLevel) {
                c.ampl = c.sustainLevel;
                c.state = ENV_SUSTAIN;
            }
            break;
        case ENV_SUSTAIN:
            break;
        case ENV_RELEASE:
            c.ampl -= c.releaseRate;
            if (c.ampl <= AMPL_MIN) {
                SD_VT("chan %2d off: envelope release reached silence\n", i);
                sd_pd_end(i, c.priority, "relfloor");
                c.active = 0;
                c.state = ENV_OFF;
                c.priority = 0;   /* reached the floor: priority 0 */
            }
            break;
        default:
            break;
        }
    }
    if (g_ctOn) ct_frame();
}

void sd_seq_frame(void);   // forward: the sequencer shares this clock

void sd_mix_render(sd_s16 *dst, int frames)
{
    memset(dst, 0, (size_t)frames * 2 * sizeof(sd_s16));
    int done = 0;
    while (done < frames) {
        // Frames remaining before the next 192 Hz boundary.
        int need = (SD_MIX_RATE - g_tickAcc + 191) / 192;
        if (need <= 0) need = 1;
        int n = frames - done;
        if (n > need) n = need;

        for (int i = 0; i < SD_CHANNELS; i++) {
            Channel &c = g_ch[i];
            if (!c.active || !c.pcm) continue;
            int envDb10 = c.ampl >> 7;   /* 0x037FBD48: asr 7, not a divide */
            int total = envDb10 + c.volDb10 + c.lfoDb10;
            if (total < -723) total = -723;
            double g = db10_to_gain(total);
            if (g <= 0.0) continue;
            int pan = c.pan + c.lfoPan;
            if (pan < 0) pan = 0;
            if (pan > 127) pan = 127;
            double gl = g * (127 - pan) / 127.0;
            double gr = g * pan / 127.0;

            sd_s16 *o = dst + (done * 2);
            for (int k = 0; k < n; k++, o += 2) {
                sd_u32 idx = (sd_u32)c.pos;
                if (idx >= c.total) {
                    if (c.loop && c.total > c.loopStart) {
                        double span = (double)(c.total - c.loopStart);
                        c.pos = c.loopStart + fmod(c.pos - c.loopStart, span);
                        idx = (sd_u32)c.pos;
                    } else {
                        SD_VT("chan %2d off: sample ran out (%u samples, "
                              "no loop)\n", i, (unsigned)c.total);
                        sd_pd_end(i, c.priority, "sampleend");
                        c.active = 0;
                        c.priority = 0;   /* stopped: priority 0 */
                        break;
                    }
                }
                double frac = c.pos - (double)idx;
                sd_s32 a = c.pcm[idx];
                sd_s32 b = (idx + 1 < c.total) ? c.pcm[idx + 1]
                         : (c.loop ? c.pcm[c.loopStart] : a);
                double s = c.hold ? (double)a : a + (b - a) * frac;
                sd_s32 l = (sd_s32)(o[0] + s * gl);
                sd_s32 r = (sd_s32)(o[1] + s * gr);
                o[0] = (sd_s16)(l < -32768 ? -32768 : (l > 32767 ? 32767 : l));
                o[1] = (sd_s16)(r < -32768 ? -32768 : (r > 32767 ? 32767 : r));
                c.pos += c.step;
            }
        }

        done += n;
        g_tickAcc += n * 192;
        while (g_tickAcc >= SD_MIX_RATE) {
            g_tickAcc -= SD_MIX_RATE;
            sd_seq_frame();
            sd_mix_frame();
        }
    }

    // Host master volume: one final scalar on the already-mixed stereo buffer.
    // This is the output stage, not the DS mixer -- every voice, envelope and
    // pan above stayed hardware-faithful; this only trims the summed result on
    // the way out (and so the .wav dump and the device hear the same level).
    // pct == 100 is a no-op fast path; pct == 0 zeroes (the old muted case).
    int pct = g_host_mute ? 0 : out_volume_pct();
    if (pct != 100) {
        for (int i = 0; i < frames * 2; i++) {
            sd_s32 s = (sd_s32)((long long)dst[i] * pct / 100);
            dst[i] = (sd_s16)(s < -32768 ? -32768 : (s > 32767 ? 32767 : s));
        }
    }

    // ---- THE AUX RENDER HOOK, lane VOICE --------------------------------
    //
    // A nullable function pointer and not a direct call, for one reason: this
    // file is linked into targets that have no network and no voice chat, and
    // a hard call would make every one of them fail the link for a feature
    // they do not build. Nothing registers it unless hal/voice_chat.cpp is in
    // the target AND the player turned voice on, so the default build runs one
    // null test per render block and nothing else.
    //
    // AFTER THE MASTER VOLUME ON PURPOSE. Everything above this line is the
    // DS's own mixer plus the host output trim, and the .wav dump downstream
    // is the record of what the game sounded like. Voice chat is not the game:
    // it has its own slider (VoiceVolume) and a player who muted the game to
    // hear his friends should still hear his friends. Running it above the
    // trim would have made SM64DS_VOLUME=0 a second mute for a control that
    // already has one.
    if (g_aux_render) g_aux_render(dst, frames);
}

void sd_mix_set_aux_render(void (*fn)(sd_s16 *, int))
{
    g_aux_render = fn;
}
