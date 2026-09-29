//cpp
// @symbol _ZN5Sound15PlaySecretSoundEP8dActor_cPt
// Sound::PlaySecretSound(dActor_c*, u16*). Soft secret chime while *counter < 0x4b,
// then the full sound once the counter is saturated. dActor_c is unused
// (presence-only in the signature).
#include "types.h"

struct dActor_c;

extern "C" int _ZN5Sound7PlaySubEjjj5Fix12IiEb(unsigned int soundID, unsigned int vol, unsigned int pan, Fix12i dist, int loop);

static const int kFullChimeDistance = 0x8777;

namespace Sound {

int PlaySecretSound(dActor_c* actor, u16* counter)
{
    int result = 0;
    (void)actor;
    if (*counter < 0x4b) {
        _ZN5Sound7PlaySubEjjj5Fix12IiEb(0x20, 0x14, 0x7f, 0x6b000, 0);
        *counter += 1;
        goto finished;
    }
    if (_ZN5Sound7PlaySubEjjj5Fix12IiEb(0x20, 0x7f, 0, kFullChimeDistance, 0))
        return 1;
finished:
    return result;
}

}
