//cpp
// @symbol _ZN3G2S12GetBG3ScrPtrEv

namespace G2S {
unsigned int GetBG3ScrPtr()
{
    int bgMode = *(volatile int *)0x04001000 & 7;
    unsigned int bgControl = *(volatile unsigned short *)0x0400100e;
    unsigned int screenBlock = (bgControl & 0x1f00) >> 8;
    switch (bgMode) {
    case 0:
    case 1:
    case 2:
        return (screenBlock << 0xb) + 0x06200000;
    case 3:
    case 4:
    case 5:
        if (bgControl & 0x80)
            return (screenBlock << 0xe) + 0x06200000;
        return (screenBlock << 0xb) + 0x06200000;
    case 6:
        return 0;
    default:
        return 0;
    }
}
}
