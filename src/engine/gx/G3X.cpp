//cpp
/* engine/gx/G3X.cpp — G3X TU (arm9 0x02055574..0x02055624)
 *
 * G3X is the 3D engine's fog and clear-color register writer set. All three
 * members are static (no this); func_020555a4 is a file-local helper that
 * snapshots the 0x80-byte register block at 0x4000380 and keeps its func_
 * name under extern "C". Written ROM-descending because mwccarm emits .text
 * in reverse source order under deferred codegen.
 */
#include "G3X.h"

extern "C" {
void MultiCopyHalf(void *dst, const void *src, unsigned int count);
void Copy32Bytes(void *src, void *dst);
void func_020555a4(void *dst);
}

// @symbol _ZN3G3X6SetFogEbiii
void G3X::SetFog(bool enable, int a, int b, int c) {
    if (enable) {
        *(volatile unsigned short *)0x400035c = (unsigned short)c;
        *(volatile unsigned short *)0x4000060 =
            (b << 8) | (a << 6) | 0x80 | (*(volatile unsigned short *)0x4000060 & ~0x3f40);
    } else {
        *(volatile unsigned short *)0x4000060 = *(volatile unsigned short *)0x4000060 & 0xcf7f;
    }
}

// @symbol _ZN3G3X11SetFogTableEPv
void G3X::SetFogTable(void *table) {
    Copy32Bytes(table, (void *)0x4000360);
}

// @symbol func_020555a4
extern "C" void func_020555a4(void *dst) {
    MultiCopyHalf(dst, (const void *)0x04000380, 0x40u);
}

// @symbol _ZN3G3X13SetClearColorEtiiib
void G3X::SetClearColor(unsigned short a, int b, int c, int d, bool e) {
    unsigned int v = (a | (b << 16)) | (d << 24);
    if (*(int *)&e) v |= 0x8000;
    *(volatile unsigned int *)0x4000350 = v;
    *(volatile unsigned short *)0x4000354 = (unsigned short)c;
}
