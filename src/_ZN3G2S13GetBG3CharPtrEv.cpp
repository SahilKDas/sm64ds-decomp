//cpp
// @symbol _ZN3G2S13GetBG3CharPtrEv

namespace G2S {
unsigned int GetBG3CharPtr()
{
    int displayControl = *(volatile int *)0x04001000;
    unsigned int bgControl = *(volatile unsigned short *)0x0400100e;
    int bgMode = displayControl & 7;
    if (bgMode >= 3) {
        if (bgMode >= 6)
            goto unavailable;
        if (bgControl & 0x80)
            goto unavailable;
    }
    return (((bgControl & 0x3c) >> 2) << 0xe) + 0x06200000;
unavailable:
    return 0;
}
}
