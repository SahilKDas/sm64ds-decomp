#!/usr/bin/env python
"""Refuse a build that seats a stack-receiver body in a pointer-to-member cell.

THE DEFECT THIS CATCHES, which nine lanes have now each found at a different
class (5ae983797 FlameChomp, 27a24ff5a ChainChomp, 651b5e853 daPgDfdr_c,
f9936e798 daGmch_c, 45ce69707 daPgMthr_c, 00732a5ab SnowmanHead and
daBgSnmBdy_c, then this lane at TreasureChest, Bird, PrincessPeach, Butterfly
and KingBobOmb):

  A ROM state record holds a pointer to member.  MSVC calls one by putting
  `this + delta` in ECX and pushing NOTHING, whether it reaches the record
  through a `call` or through a tail `jmp`.  A port seat that writes a __cdecl
  word into that record -- one of hal/faces_sync_gen.cpp's flat C faces, or a
  matched body taken raw -- gives the body a receiver it has to read off the
  caller's stack, and nobody put one there.  It shows up as a junk receiver, a
  null receiver, or control landing on a data word.

  The seat header is usually the thing that is wrong and it is wrong the same
  way each time: it reasons that the dispatcher is a tail jump, so the caller's
  own argument is still in place.  That is true of a FLAT C dispatcher whose
  first stack argument is the receiver (BabyPenguin's host copies, the path
  lift's func_ov002_020efa54, the puzzle piece's func_ov064_0211982c), and it
  is false as soon as the dispatcher is a __thiscall member, and false again as
  soon as /O2 inlines it into a caller in the same translation unit.

THE CRITERION, and why it is mechanical.  Every seat in port/ writes its cells
out of one static table in its own object's .rdata, so the code word that ends
up in each cell is IN THE BUILT IMAGE and can be read back.  For each seat
table this file names, the guard reads every code word out of the image, walks
the body's prologue, and asks where the receiver comes from:

  ECX      the body reads ecx before anything overwrites it: a __thiscall
           member, or the __fastcall thunk this campaign seats over a face.
  STACK    the body never reads ecx and does read [ebp+8] / [esp+4].
  NEITHER  neither: the body ignores its receiver.

A __fastcall face that ALSO takes a stack argument reads both, and ECX wins:
what decides is where the RECEIVER comes from.  A STACK word in a table this
file marks ECX is the defect, and the guard refuses.

The tables marked CDECL are the adjudicated exceptions, each with the flat C
dispatcher that does push the receiver named beside it.  A thunk on one of
those would break what works, so they are recorded here rather than left for
the next lane to rediscover.

usage: pmf_guard.py [--root <tree>] [--exe <exe>] [--map <map>] [--selftest]
                    [--list]
"""
import argparse
import bisect
import os
import re
import struct
import sys

try:
    import capstone
except ImportError:
    sys.stderr.write("pmf_guard: capstone is not installed\n")
    raise SystemExit(2)


# ---- the ledger -----------------------------------------------------------
# One row per seat table, matched against the map by REGEX because MSVC
# decorates a file-scope static with its own unnamed struct tag.  ECX rows are
# checked; CDECL rows are the adjudicated exceptions and are only reported.
LEDGER = [
    ("ECX", r"^\?g_treasure_chest_states@@",
     "TreasureChest: ?SetState@TreasureChest@@QAEXH@Z calls through "
     "_data_ov064_0211c98c with ecx = this + delta, ?CallStateBehavior tail "
     "jumps the same way; both are __thiscall members, so no receiver is ever "
     "on the stack"),
    ("ECX", r"^\?g_princess_peach_states@@",
     "PrincessPeach: ?CallStateInit and ?CallStateBehavior both tail jump "
     "through the record at +0x350 with ecx = this + delta, the shape "
     "00732a5ab fixed for SnowmanHead one overlay over"),
    ("ECX", r"^\?g_butterfly_states@@",
     "Butterfly: ?Behavior@Butterfly@@UAEHXZ carries the call inline through "
     "_data_ov100_02148628 with ecx = this + delta and nothing pushed"),
    ("ECX", r"^\?g_bird@@",
     "Bird: ?Behavior@Bird@@UAEHXZ calls through _data_ov009_02113c48 with "
     "ecx = this + delta and nothing pushed"),
    ("ECX", r"^\?g_king_states@@",
     "KingBobOmb: five of the class's own matched bodies inline the state "
     "change and call through _data_ov078_02126ffc with ecx = this + delta; "
     "_KingBobOmb_SetState carries ecx on its tail-jump path too"),
    ("ECX", r"^\?g_smb_cells@@",
     "daBgSnmBdy_c: ?Behavior and ?InitResources both carry the call inline "
     "with ecx = this + delta (00732a5ab)"),
    ("ECX", r"^\?g_smh_cells@@",
     "SnowmanHead: ?SetState calls ?CallStateInit with nothing pushed and "
     "both Call* helpers tail jump with ecx = this + delta (00732a5ab)"),

    # ---- the minigame framework base, run link100 lane PMFSWEEP2 -----------
    ("ECX", r"^\?seats_ecx@\?1\?\?port_mg_base_writer_seat@@",
     "dMgState_c: these twenty-nine pair globals are copied by the state "
     "bodies into the framework message object at +0x08 and +0x10, and those "
     "two fields have exactly two readers in the image, "
     "?Behavior@dMgState_c@@QAEXXZ (+0x16 mov eax,[esi+8]; +0x20 mov ecx,"
     "[esi+0xc]; add ecx,esi; jmp eax) and ?Render@dMgState_c@@QAEXXZ (+0x8 "
     "mov edx,[eax+0x10]; +0xf mov ecx,[eax+0x14]; add ecx,eax; jmp edx). "
     "Both are __thiscall members with NO stack argument and their callers "
     "push nothing (?BeforeBehavior@dScMgBase_c@@UAEHXZ+0x96 lea ecx,"
     "[esi+0xcc]; call), so the word at [esp+4] is the caller's saved edi"),
    ("ECX", r"^\?cells_490@\?1\?\?port_mg_framework_tables_seat@@",
     "data_ov004_020bf490: src/actors/unit020b2c84.cpp dispatches this table "
     "itself with a real call, ecx = this and nothing pushed (lane MGWRITER "
     "measured it off the TU's own listing and seated __fastcall faces)"),

    ("CDECL", r"^\?seats_cdecl@\?1\?\?port_mg_base_writer_seat@@",
     "dMgState_c: twenty of these are group A, the setter's own table, and "
     "the only reader of the object's +0x00 field is the host copy "
     "__ZN10dMgState_c8SetStateEi, whose mgbase_dispatch_seated is a plain "
     "cdecl call that PUSHES the receiver. The other seven land in the "
     "020b3278 object, whose two readers are the FLAT C dispatchers "
     "_func_ov004_020b321c and _func_ov004_020b31b4, f(self) tail jumps that "
     "leave the caller's own pushed argument at [esp+4]"),
    ("CDECL", r"^\?cells_field@\?1\?\?port_mg_framework_tables_seat@@",
     "data_ov004_020bf428 and _020bf4f8: func_ov004_020b3278 copies these "
     "into the 020b3278 object at +0x00 and +0x08, which _func_ov004_020b321c "
     "and _func_ov004_020b31b4 read, and both are flat f(self) tail jumps"),
    ("CDECL", r"^\?seats@\?1\?\?port_mg_framework_states_seat@@",
     "data_ov004_020beb88 and _020beb98: the readers are the matched TUs "
     "src/minigames/d_s_mg_base.cpp and src/minigames/d_s_mg_base.cpp, each a "
     "flat f(self) that loads the pair out of the table and tail jumps with "
     "the frame restored (lane PMFB3 measured both listings)"),

    ("CDECL", r"^\?g_bp_cells@@",
     "BabyPenguin: the dispatchers are the host copies "
     "BabyPenguin_StateEnter.cpp and BabyPenguin_StateTick.cpp, which call "
     "the cell as a plain function pointer and DO push the receiver, so a "
     "thunk here breaks what works (00732a5ab left these alone on purpose)"),
    ("CDECL", r"^\?seats@\?1\?\?port_pathlift_states_seat@@",
     "dPathLiftActor_c: the three TICK halves already carry __fastcall faces; "
     "the three ENTER halves are read by NOTHING but func_ov002_020efa54, a "
     "flat C dispatcher f(self, state) that tail jumps, so [esp+4] really is "
     "the receiver there (proved by a byte scan of .text for every reference "
     "to _data_ov002_0210af2c+0/+4)"),
    ("CDECL", r"^\?g_piece_states@@",
     "BowserPuzzlePiece: the .b half already carries a __fastcall face; the "
     ".a half's only reader is func_ov064_0211982c, a flat C dispatcher "
     "f(self, state) that tail jumps"),

    # ---- run link100 lane PMFSWEEP3 ---------------------------------------
    ("ECX", r"^\?g_rabbit_states@@",
     "daMip_c (MIPS the rabbit): the class dispatches its own state record and "
     "every one of its sixteen dispatch sites is a __thiscall member that "
     "pushes nothing -- ?Behavior@daMip_c@@UAEHXZ +0x2f6 mov [edi+0x364],"
     "0x18a6b2c; +0x300 mov eax,[0x18a6b2c]; +0x309 mov ecx,edi; +0x30b add "
     "ecx,[0x18a6b30]; +0x311 call eax, and the same three moves again in "
     "?InitResources +0x398, ?Render +0x22a, ?StateCaughtInit, "
     "?StateCaughtMain, ?StateFleeInit, ?StateFleeMain, ?StateReleasedMain, "
     "?StateRestInit, ?StateRestMain, ?StateSaveTalkMain, ?StateStartleInit, "
     "?StateStartleMain and ?StateTalkMain. __sinit_ov085_0212f5ec copies the "
     "seat's source records into the runtime ones those sites read, so the "
     "seat's word is the word they call"),
    ("ECX", r"^\?g_ov077_seats@",
     "ov077 HeaveHo's per-frame halves: ?Behavior@HeaveHo@@UAEHXZ +0x51 mov "
     "ecx,[edi+0x3fc]; +0x5a mov eax,[ecx+8]; +0x61 mov ecx,[ecx+0xc]; +0x64 "
     "add ecx,edi; +0x66 call eax -- the dispatcher is INLINE in a member and "
     "pushes nothing. The rest of this table is the adjudicated __cdecl set "
     "below: the enter halves and Lakitu's and Spiny's tick halves are reached "
     "only by the flat f(self) tail jumps _func_ov077_02124754 (va 00529ab0), "
     "_02124718 (00529a90), _02125e5c (00529af0), _02125e20 (00529ad0) and "
     "_02126d5c (0052adc0), each `mov eax,[ebp+8] / mov ecx,[rec+4] / add "
     "ecx,eax / pop ebp / jmp`, so [esp+4] really is the receiver there",
     r"^_func_ov077_(?!02126640|0212679c|02126ad0|02126a50|021269a8)"),
    ("ECX", r"^\?g_ov060_states@",
     "ov060's Bowser pack: the four tables dispatched by __thiscall members "
     "(?Behavior and ?InitResources of BowserFire, ?Behavior of "
     "BowserSkyPlatform, and the Bowser tail) already carry the ov60_* "
     "__fastcall faces and must keep them. The raw matched bodies in this "
     "table are the adjudicated __cdecl set below: _func_ov060_021128c0 "
     "(va 00505da0) decodes the record by hand and dispatches it at +0x8f "
     "`push edx / call eax`, PUSHING the receiver, and HOST COPY 1 "
     "func_ov060_02112434 in port/unmatched/Ov060_StateDispatch.cpp spells the "
     "same call as ((void (*)(char *))e->fn)(thiz + (e->adj >> 1))",
     r"^_func_ov060_(?!02115c1c|02115d50|02115d68|021167c8|021167ec|"
     r"021168c4|021169b0|021169f8|02116b18|02116b68|02116c68|02116d78|"
     r"02116f74|02116f90|021171e8|0211722c|0211747c|02117db8|021180e0|"
     r"021181b4)"),

    ("ECX", r"^\?g_scuttlebug_sources@@",
     "Scuttlebug: all eighteen cells take their receiver in ecx. The nine MAIN "
     "cells go through ?Behavior@Scuttlebug@@UAEHXZ +0x1 mov esi,ecx; +0x10 mov "
     "eax,[esi+0x380]; +0x19 mov ecx,[eax+0xc]; +0x1c mov eax,[eax+8]; +0x1f add "
     "ecx,esi; +0x21 call eax. The nine ENTER cells were excused here as __cdecl "
     "on the reading that the flat _Scuttlebug_SetState (va 005eb460) is their "
     "only reader. It is not, and this file's own docstring says why: /O2 inlines "
     "that dispatcher into fourteen sites in the same TU, thirteen of them `lea "
     "ecx,[ecx+this]; call dword ptr [cell]` with nothing pushed. Run link100 lane "
     "HMC1 read every absolute reference into data_ov071_02122fa8 back out of the "
     "image and thunked the enter half too; both flat dispatchers set ecx as well "
     "as leaving the receiver on the stack, so they are unaffected"),
    ("CDECL", r"^\?g_crate_states@@",
     "Crate: the readers are _Crate_SetState (va 0052ce50, +0x21 jmp eax) and "
     "_func_ov098_02138b70 (va 0052ce80, +0x1e jmp eax), both flat f(self) "
     "tail jumps that load the pair out of the table with the receiver in "
     "[ebp+8] and leave it at [esp+4]"),
    ("ECX", r"^\?g_ukiki_cells@@",
     "daMky_c: all twenty-two cells take their receiver in ecx. The eleven "
     "TICK cells go through ?Behavior@daMky_c@@UAEHXZ +0x1f5 mov eax,"
     "[edi+0x3a4]; +0x1fb mov ecx,[eax+0xc]; +0x1fe mov eax,[eax+8]; +0x201 "
     "add ecx,edi; +0x203 call eax, and the flat tail jumps "
     "_func_ov030_021141a8 and _02114134, _02113324, _02113d20, _02113ff0 "
     "ride the receiver through while also setting ecx. The eleven ENTER "
     "cells were excused here as __cdecl on the reading that those flat "
     "jumps are their only readers. They are not: run link100 lane UKIKI2 "
     "read every absolute reference into data_ov030_02115e0c back out of "
     "the image and found ?InitResources@daMky_c@@UAEHXZ +0x1d0 and +0x205 "
     "inline two more dispatches, `lea ecx,[ecx+this]; call dword ptr "
     "[cell]` with nothing pushed -- the Scuttlebug shape 65deff04d fixed "
     "at the sibling class the same night. Both the flat dispatcher and the "
     "inlined calls set ecx, so the enter half is thunked too and this "
     "table leaves the exception list"),
]


# ---- image ----------------------------------------------------------------
MAPROW = re.compile(
    r"\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{8})\s+(\S*)\s*(\S+)?\s*$")


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    off = pe + 24 + optsz
    out = []
    for i in range(nsec):
        s = off + i * 40
        out.append((struct.unpack_from("<I", data, s + 12)[0],
                    struct.unpack_from("<I", data, s + 8)[0],
                    struct.unpack_from("<I", data, s + 20)[0],
                    struct.unpack_from("<I", data, s + 16)[0]))
    return base, out


class Image(object):
    def __init__(self, exe, mp):
        self.data = open(exe, "rb").read()
        self.base, self.secs = sections(self.data)
        self.syms = []
        base = 0x400000
        started = False
        with open(mp, encoding="utf-8", errors="replace") as f:
            for line in f:
                if "Preferred load address is" in line:
                    base = int(line.split()[-1], 16)
                if "Publics by Value" in line:
                    started = True
                    continue
                if not started:
                    continue
                m = MAPROW.match(line)
                if m:
                    obj = m.group(4) or m.group(3)
                    self.syms.append((int(m.group(2), 16) - base, m.group(1), obj))
        self.syms.sort()
        self.keys = [s[0] for s in self.syms]
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        va, vsz, raw, rsz = self.secs[0]
        self.text = (va, va + max(vsz, rsz))

    def off(self, rva):
        for va, vsz, raw, rsz in self.secs:
            if va <= rva < va + max(vsz, rsz):
                d = rva - va
                if d < rsz:
                    return raw + d
        return None

    def code(self, rva, n):
        o = self.off(rva)
        return b"" if o is None else self.data[o:o + n]

    def name_at(self, rva):
        i = bisect.bisect_right(self.keys, rva) - 1
        if i < 0:
            return ("?", "?", 0)
        r, n, o = self.syms[i]
        return (n, o, rva - r)


# ---- the classifier -------------------------------------------------------
STACKOP = re.compile(r"\[ebp \+ 8\]|\[esp \+ 4\]")
ECXWORD = re.compile(r"\becx\b")
READS_ITS_OPERAND = ("push", "jmp", "call", "test", "cmp")
KILLS_ITS_FIRST = ("mov", "lea", "pop", "movzx", "movsx", "xor")


def receiver_of_bytes(md, blob, rva, limit=60):
    stack_read = False
    ecx_dead = False
    saw_call = False
    for i in list(md.disasm(blob, rva))[:limit]:
        ops = i.op_str
        if STACKOP.search(ops):
            stack_read = True
        if ECXWORD.search(ops) and not ecx_dead:
            if i.mnemonic in READS_ITS_OPERAND:
                return "ECX"
            if ops.split(",")[0].strip() != "ecx":
                return "ECX"
        if ops.split(",")[0].strip() == "ecx" and i.mnemonic in KILLS_ITS_FIRST:
            ecx_dead = True
        if i.mnemonic == "ret":
            break
        # A TAIL JUMP ENDS THE BODY (run link100 lane PMFSWEEP2). Without this
        # the walk ran off the end of a short forwarder, through the int3
        # padding and into whatever function the linker put next, and read that
        # one's prologue instead. It was not academic: every one of the six
        # counting wrappers in port/unmatched/MgBase_StateSetter.cpp is
        # `push ebp; mov ebp,esp; inc <counter>; pop ebp; jmp <body>`, reads
        # nothing itself, and classified as ECX purely because the NEXT wrapper
        # in the image opens with `push ecx`. A forwarder like that in an ECX
        # table is the defect this guard exists for -- it hands the body a
        # stack nobody wrote -- so reading it as ECX is exactly the wrong
        # answer. The caller re-walks at the target, which is what a tail jump
        # means.
        #
        # ONLY WHEN THE BODY IS A PURE FORWARDER, though, and that qualifier is
        # a measured one rather than caution: ?port_bird_state2@@YIXPAX0@Z is a
        # __fastcall refusal stub that ignores its receiver, prints, and tail
        # jumps into abort() -- and abort's own prologue reads [esp+4], so
        # following that jump reported the stub as a stack reader and refused a
        # build that is correct. A body that has already made a call has used
        # its frame and is not handing a receiver on, so the walk stops there
        # with what it saw, which for that stub is NEITHER.
        if i.mnemonic == "call":
            saw_call = True
        if i.mnemonic == "jmp":
            if re.match(r"^0x[0-9a-f]+$", ops) and not saw_call:
                return ("STACK" if stack_read else "TAILJUMP:" + ops)
            break   # an indirect or post-call tail jump: nothing to follow
    return "STACK" if stack_read else "NEITHER"


def receiver_of(img, rva, hops=4):
    """The receiver class of the body at rva, following tail jumps.

    A tail jump is a continuation: the body that ends in one takes its receiver
    however the body it jumps to takes it, because the frame is handed over
    untouched. Bounded at four hops so a jump cycle cannot spin."""
    seen = set()
    for _ in range(hops):
        if rva in seen:
            break
        seen.add(rva)
        k = receiver_of_bytes(img.md, img.code(rva, 760), rva)
        if not k.startswith("TAILJUMP:"):
            return k
        nxt = int(k.split(":", 1)[1], 16)
        if not (img.text[0] <= nxt < img.text[1]):
            return "NEITHER"
        rva = nxt
    return "NEITHER"


# ---- the census -----------------------------------------------------------
def cells_of(img, sym_rva, end_rva):
    """Every code word stored in [sym_rva, end_rva)."""
    out = []
    off = img.off(sym_rva)
    if off is None:
        return out
    n = end_rva - sym_rva
    blob = img.data[off:off + n]
    for k in range(0, len(blob) - 3, 4):
        w = struct.unpack_from("<I", blob, k)[0]
        if w < img.base:
            continue
        r = w - img.base
        if img.text[0] <= r < img.text[1]:
            out.append((sym_rva + k, r))
    return out



def ledger():
    """The ledger rows, normalised to (kind, pattern, why, cdecl_ok).

    cdecl_ok is the fourth field an ECX row may carry (run link100 lane
    PMFSWEEP3): a regex over BODY symbol names whose cells are the adjudicated
    __cdecl exceptions INSIDE a checked table.  Several ROM tables mix the two
    conventions because two dispatchers read the same array -- a flat f(self)
    tail jump for one half, an inlined member for the other -- and a table with
    no way to say so could only be left out of the ledger entirely or added as
    a blanket CDECL row, which stops checking the half that must stay in ECX.
    The exception is spelled as EXACT body names (or a negative lookahead over
    them) rather than a family prefix on purpose: dropping a __fastcall face
    back to its raw body renames the cell's body to something the list does not
    allow, so that regression still refuses."""
    for row in LEDGER:
        kind, pat, why = row[0], row[1], row[2]
        yield kind, pat, why, (row[3] if len(row) > 3 else None)


def cdecl_ok_for(name):
    for kind, pat, why, ok in ledger():
        if re.match(pat, name):
            return ok
    return None


def refusals(rows):
    """Split census rows into (refused, excused-by-name, adjudicated tables).

    Factored out of main so the self-test can drive it: gutting the criterion
    is caught by the prologue fixtures, and gutting THIS is caught by the
    verdict fixtures below."""
    bad, excused = [], []
    for r in rows:
        if r[0] != "ECX" or r[6] != "STACK":
            continue
        ok = cdecl_ok_for(r[1])
        if ok and re.match(ok, r[5][0]):
            excused.append(r)
        else:
            bad.append(r)
    return bad, excused

def census(img):
    datasyms = [(r, n, o) for r, n, o in img.syms
                if not (img.text[0] <= r < img.text[1])]
    datasyms.sort()
    rows = []
    for idx, (rva, name, obj) in enumerate(datasyms):
        for kind, pat, why, ok in ledger():
            if re.match(pat, name):
                end = rva + 0x400
                for j in range(idx + 1, len(datasyms)):
                    if datasyms[j][0] > rva:
                        end = datasyms[j][0]
                        break
                for at, target in cells_of(img, rva, end):
                    rows.append((kind, name, obj, at, target,
                                 img.name_at(target), receiver_of(img, target),
                                 why))
                break
    return rows


# ---- the self-test --------------------------------------------------------
# Three real prologues, as bytes, so the check is proved without the image.
FLAT_FACE = bytes(bytearray([
    0x55,                          # push ebp
    0x8B, 0xEC,                    # mov ebp, esp
    0x8B, 0x4D, 0x08,              # mov ecx, dword ptr [ebp+8]
    0x5D,                          # pop ebp
    0xE9, 0x00, 0x00, 0x00, 0x00,  # jmp <member>
]))
CDECL_BODY = bytes(bytearray([
    0x55,                          # push ebp
    0x8B, 0xEC,                    # mov ebp, esp
    0x56,                          # push esi
    0x8B, 0x75, 0x08,              # mov esi, dword ptr [ebp+8]
    0xC3,                          # ret
]))
FASTCALL_THUNK = bytes(bytearray([
    0x51,                          # push ecx
    0xE8, 0x00, 0x00, 0x00, 0x00,  # call <face>
    0x83, 0xC4, 0x04,              # add esp, 4
    0xC3,                          # ret
]))
FASTCALL_WITH_STACK_ARG = bytes(bytearray([
    0x55,                          # push ebp
    0x8B, 0xEC,                    # mov ebp, esp
    0xFF, 0x75, 0x08,              # push dword ptr [ebp+8]   the int argument
    0x51,                          # push ecx                 the receiver
    0xE8, 0x00, 0x00, 0x00, 0x00,  # call <body>
    0xC2, 0x04, 0x00,              # ret 4
]))
THISCALL_MEMBER = bytes(bytearray([
    0x8B, 0xC1,                    # mov eax, ecx
    0x8B, 0x50, 0x10,              # mov edx, dword ptr [eax+0x10]
    0xC3,                          # ret
]))
# A counting wrapper that reads NOTHING of its own and tail jumps, the shape
# every bw_ row in port/unmatched/MgBase_StateSetter.cpp has. Its receiver is
# whatever the body it jumps to takes, so the classifier must report the jump
# rather than walk off the end of it into the next function in the image.
TAILJUMP_WRAPPER = bytes(bytearray([
    0x55,                          # push ebp
    0x8B, 0xEC,                    # mov ebp, esp
    0xFF, 0x05, 0x00, 0x10, 0x40, 0x00,   # inc dword ptr [0x401000]
    0x5D,                          # pop ebp
    0xE9, 0xFB, 0x0F, 0x00, 0x00,  # jmp 0x402010
]))

SELFTESTS = [
    ("a flat C face from hal/faces_sync_gen.cpp", FLAT_FACE, "STACK"),
    ("a raw matched cdecl body", CDECL_BODY, "STACK"),
    ("the __fastcall thunk this campaign seats", FASTCALL_THUNK, "ECX"),
    ("a __fastcall face that also takes a stack argument",
     FASTCALL_WITH_STACK_ARG, "ECX"),
    ("a __thiscall member", THISCALL_MEMBER, "ECX"),
    ("a counting wrapper that tail jumps", TAILJUMP_WRAPPER, "TAILJUMP:0x40200a"),
]



# ---- the verdict fixtures -------------------------------------------------
# The prologue fixtures above prove the CRITERION.  These prove the WIRING:
# that a stack-receiver cell in a checked table actually reaches the refusal,
# that the fourth-field exception excuses only the body it names, and that a
# CDECL table is reported rather than refused.  Without them, emptying the
# refusal list in main() left every prologue fixture green and shipped a build
# with a raw cell in it (measured, run link100 lane PMFSWEEP3).
def _row(kind, table, body, klass):
    return (kind, table, "fixture.obj", 0x1000, 0x2000, (body, "fixture.obj", 0),
            klass, "fixture")


VERDICTS = [
    ("a stack-receiver cell in a checked table refuses",
     [_row("ECX", "?g_rabbit_states@@3QBU", "_func_ov085_0212b4b4", "STACK")], 1, 0),
    ("an ECX cell in a checked table passes",
     [_row("ECX", "?g_rabbit_states@@3QBU", "?rb_0212b4b4@@YIXPAX0@Z", "ECX")], 0, 0),
    ("the named __cdecl exception inside a checked table is excused",
     [_row("ECX", "?g_ov077_seats@?A0x1@@3QBU", "__ZN7daJgm_c15UpdateSpitStateEv", "STACK")], 0, 1),
    ("a body the exception does NOT name still refuses",
     [_row("ECX", "?g_ov077_seats@?A0x1@@3QBU", "_func_ov077_02126640", "STACK")], 1, 0),
    ("a stack-receiver cell in an adjudicated table is only reported",
     [_row("CDECL", "?g_bp_cells@@3QBU", "_func_ov072_02121c94", "STACK")], 0, 0),
]


def verdict_selftest():
    bad = 0
    for what, rows, want_bad, want_excused in VERDICTS:
        got_bad, got_excused = refusals(rows)
        ok = len(got_bad) == want_bad and len(got_excused) == want_excused
        print("  %s %-58s want %d/%d got %d/%d" % (
            "ok " if ok else "FAIL", what, want_bad, want_excused,
            len(got_bad), len(got_excused)))
        if not ok:
            bad += 1
    if bad:
        print("pmf_guard --selftest: %d of %d verdicts are WRONG, so the guard "
              "would not refuse what it classifies" % (bad, len(VERDICTS)))
    return bad


def selftest():
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    bad = 0
    for what, blob, want in SELFTESTS:
        got = receiver_of_bytes(md, blob, 0x401000)
        ok = "ok " if got == want else "FAIL"
        print("  %s %-52s want %-7s got %s" % (ok, what, want, got))
        if got != want:
            bad += 1
    if bad:
        print("pmf_guard --selftest: %d of %d classifications are WRONG, so the "
              "guard cannot see the defect it exists for" % (bad, len(SELFTESTS)))
        return 1
    if verdict_selftest():
        return 1
    print("pmf_guard --selftest OK: %d prologues and %d verdicts, every one as "
          "the campaign's own evidence says" % (len(SELFTESTS), len(VERDICTS)))
    return 0


# ---- main -----------------------------------------------------------------
def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--exe")
    ap.add_argument("--map")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()

    exe = a.exe or os.path.join(a.root, "build", "port", "walk_window.exe")
    mp = a.map or os.path.join(a.root, "build", "port", "walk_window.map")
    if not os.path.exists(exe) or not os.path.exists(mp):
        print("pmf_guard: no built image at %s -- nothing to check" % exe)
        return 0

    rc = selftest()
    if rc:
        return rc

    img = Image(exe, mp)
    rows = census(img)
    seen = set(name for _, name, _, _, _, _, _, _ in rows)
    missing = []
    for kind, pat, why, ok in ledger():
        if not any(re.match(pat, n) for n in seen):
            missing.append(pat)
    if missing:
        print("pmf_guard REFUSES: %d seat table(s) in the ledger are not in the "
              "map, so the ledger has gone stale and the check is not running "
              "on them:" % len(missing))
        for m in missing:
            print("    %s" % m)
        return 1

    checked = [r for r in rows if r[0] == "ECX"]
    adjudicated = [r for r in rows if r[0] == "CDECL"]
    bad, excused = refusals(rows)

    if a.list:
        for kind, name, obj, at, target, tn, klass, why in rows:
            print("%-6s %08x  %-44s -> %-8s %s+0x%x [%s]" % (
                kind, img.base + at, name[:44], klass, tn[0], tn[2], tn[1]))

    if bad:
        print("pmf_guard REFUSES: %d cell(s) in a pointer-to-member table hold "
              "a body that reads its receiver off the caller's stack. The call "
              "site passes it in ECX and pushes nothing, so the body will read "
              "whatever the caller last spilled." % len(bad))
        for kind, name, obj, at, target, tn, klass, why in bad:
            print("")
            print("  %s  [%s]" % (name, obj))
            print("    cell at %08x holds %08x = %s+0x%x [%s]" % (
                img.base + at, img.base + target, tn[0], tn[2], tn[1]))
            print("    its prologue reads [ebp+8]/[esp+4] and never reads ecx")
            print("    why this table needs ECX: %s" % why)
        print("")
        print("Fix: seat a __fastcall thunk that names the body, the shape "
              "5ae983797 / 27a24ff5a / 651b5e853 / f9936e798 / 45ce69707 / "
              "00732a5ab all use. If the cell is really reached only by a flat "
              "C dispatcher that pushes the receiver, move its table to a "
              "CDECL row in this file's ledger WITH the dispatcher named.")
        return 1

    print("pmf_guard OK: %d cell(s) across %d pointer-to-member table(s) all "
          "take their receiver in ECX; %d cell(s) across %d table(s) are the "
          "adjudicated __cdecl exceptions, %d of them named cell by cell inside "
          "a checked table." % (
              len(checked), len(set(r[1] for r in checked)),
              len(adjudicated) + len(excused),
              len(set(r[1] for r in adjudicated)) +
              len(set(r[1] for r in excused)), len(excused)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
