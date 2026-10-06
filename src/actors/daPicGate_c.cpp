//cpp
/* ov080/daPicGate_c -- picture gate (PICTURE_GATE 307).
 *
 * param1 low nibble is the width in 100-unit steps, minus one. The next
 * nibble is the height. Bits 8-12 are the picture id. Bits 13-14 pick one
 * of the four rows in the state table.
 *
 * State 0 is the ripple mesh: fill the grid, count the wave down, draw a
 * triangle strip. State 1 fills that mesh and pulses the center, then each
 * frame slides picture 7 from mClosedPosX by 2050 once star 1 of level
 * 0x12 is in, and latches bit 0x40000 of data_0209caa0[2]. The same shift
 * is what daChScene_c applies to an exit whose high param byte is 0x12.
 * Picture 4 in state 1 clears the two clip-enable flags. States 2 and 3
 * are the flat 2x2 quad; their other handlers are outside this run.
 *
 * The grid is square (mCols is copied from mRows) and indexed with mRows
 * as the stride. Vertex::color is the packed GX normal, not a color: the
 * strip writer stores it at 0x04000484.
 *
 * The five handlers keep func_ov080_* names: the state-table descriptors
 * relocate to those symbols. #pragma defer_codegen off keeps .text in
 * source order. The destructor pair and g_profile_PICTURE_GATE are outside
 * this run. The abutting registry factory daPicGate_c_classInit
 * (0x02126f8c) is written last, built by hand (see the note above it).
 * SetRanges is the int adapter; dActor_c.h does not declare the member.
 *
 * deslop leftovers:
 * - func_ov080_021264ec and func_ov080_021269b8: mWavePhase += phaseStep
 *   differs by 6 words. The add that matches reuses the r5+0x100 base
 *   already formed for mNumCells; += addresses mWavePhase at 0x1b4.
 * - InitResources: a picture-id local, and declaring the default wave
 *   row as a WaveParams, shrank the function from 0x2ec to 0x2b4. The
 *   bit extracts stay written out, and the row stays an untyped object
 *   whose address is cast.
 * - func_ov080_0212677c: for-loops over the columns and the rows grew
 *   it from 0x23c to 0x24c. The entry test is still n = mCols; i = 0;
 *   n = n - 1, so the subtract consumes that n, and both loops stay
 *   do/while.
 */

#pragma defer_codegen off

#include "daPicGate_c.h"
#include "common.h"

/* Geometry command ports. Plain stores, not volatile: a volatile port
 * reloads where the cartridge keeps the value in a register. */
#define G3_MTX_MODE     (*(int *)0x04000440)
#define G3_MTX_PUSH     (*(int *)0x04000444)
#define G3_MTX_POP      (*(int *)0x04000448)
#define G3_MTX_SCALE    (*(int *)0x0400046c)
#define G3_NORMAL       (*(int *)0x04000484)
#define G3_TEXCOORD     (*(int *)0x04000488)
#define G3_VTX_16       (*(int *)0x0400048c)
#define G3_LIGHT_VECTOR (*(int *)0x040004c8)
#define G3_LIGHT_COLOR  (*(int *)0x040004cc)
#define G3_BEGIN        (*(int *)0x04000500)
#define G3_END          (*(int *)0x04000504)

enum {
    kCell = 0x64000,         /* 100.0, one width or height step */
    kSlide = 0x802000,       /* 2050.0 */
    kSlideStep = 0x13e72,
    kOpened = 0x40000,
    kFlatNormal = 0x1ff00000,
    kTexSpan = 0x80000,
    kRangePad = 0xc8000,     /* 200.0, added to the clip radius */
    kFar = 0x1964000,        /* 6500.0, clip distance and far distance */
    kPicSlide = 7,
    kPicUnclipped = 4,
    kGateLevel = 0x12,
    kGateStar = 1,
    kMtxPosVec = 2,          /* GX_MTXMODE_POSITION_VECTOR */
    kMtxPos = 1,             /* GX_MTXMODE_POSITION */
    kScale = 0x20000,        /* 32.0, the three MTX_SCALE parameters */
    kTriStrip = 2,
    kClipEnable = 3
};

extern "C" {
extern int IsStarCollectedInLevel(signed char levelID, int starID);
extern int data_0209caa0[];
extern void MulMat4x3Mat4x3(const int *a, const int *b, int *out);
extern Matrix4x3 data_0209b3ec;
extern unsigned short DecIfAbove0_Short(unsigned short *p);
extern void _ZN8dActor_c9SetRangesE5Fix12IiES1_S1_S1_(
    void *self, int offsetY, int radius, int clip, int far);
extern u8 data_ov080_02127714[];
extern void func_020553a4(void *mtx);
extern void *data_ov080_02127834;
extern void func_0203cbc0(void *a);
/* daPicGate_c_classInit builds the object by hand; see the note above it. */
extern void *_ZN8dActor_cC2Ev(void *self);
}

/* The cartridge's vtable label is the address point (slot 0), and the
   vtable itself is emitted with the destructor, outside this file. */
extern int _ZTV11daPicGate_c[];
namespace Memory { void *operator_new2(unsigned int size); }

/* Call-site names. The state-table handlers keep their func_ov080_* names;
 * the helpers are daPicGate_c members declared on the class and defined in
 * src/game/actors/d_a_pic_gate.cpp. */
#define HitTest func_ov080_021256f8
#define RippleHeight func_ov080_02125bb0
#define BuildNormals func_ov080_02125940
#define FlattenFrame func_ov080_02125af0
#define PlaceCorners func_ov080_02126124
#define BeginWave func_ov080_02125de0
#define DrawFlat func_ov080_02125fd0
#define LoadMaterial func_ov080_02125460
#define BuildGateMatrix func_ov080_0212555c
#define LoadTexture func_ov080_02125630
#define LoadMtx43 func_020553a4
#define SetRanges _ZN8dActor_c9SetRangesE5Fix12IiES1_S1_S1_
#define kSave data_0209caa0
#define kStates data_ov080_02128628
#define kDefaultWave data_ov080_02127834

extern daPicGate_c::State data_ov080_02128628[];
int ApproachLinear(int &ref, int target, int step);

/* Rows for widths 3, 4, 5, 6, 7, 8 and 16: 13, 14, 16, 20, 20, 25, 30. */
#define kGridRows data_ov080_02127714

/* One vertex of a strip: texcoord, packed normal, then the 4.12 position
 * as two VTX_16 writes. vx/vy/vz/s0/s1/s2 are the caller's locals. */
#define EMIT_VTX(v) \
    G3_TEXCOORD = (v)->texCoord; \
    G3_NORMAL = (v)->color; \
    vx = (v)->x; \
    vy = (v)->y; \
    vz = (v)->z; \
    s0 = (s16)(vx >> 8); \
    s1 = (s16)(vy >> 8); \
    s2 = (s16)(vz >> 8); \
    G3_VTX_16 = (u16)s0 | ((u16)s1 << 16); \
    G3_VTX_16 = (u16)s2

// @symbol func_ov080_021264ec
/* State 1 behavior. Picture 7 slides open; every picture then ripples. */
extern "C" {
void func_ov080_021264ec(daPicGate_c *self)
{
    if ((u8)((self->param1 >> 8) & 0x1f) == kPicSlide &&
        !(kSave[2] & kOpened) &&
        IsStarCollectedInLevel(kGateLevel, kGateStar)) {
        if (ApproachLinear(self->mPosX, self->mClosedPosX + kSlide, kSlideStep))
            kSave[2] |= kOpened;
        self->BuildGateMatrix();
    }
    self->HitTest();
    int i;
    for (i = 0; i < self->mNumCells; i++) {
        daPicGate_c::Vertex *v = &self->mCells[i];
        v->z = self->RippleHeight(v->dist);
    }
    self->BuildNormals();
    self->FlattenFrame();
    self->mWavePhase = self->mWavePhase + self->mWaveParams->phaseStep;
}
}

// @symbol func_ov080_021265ec
/* State 1 init. Even grid, flat +Z normal, then a wave at the centre. */
extern "C" {
void func_ov080_021265ec(daPicGate_c *self)
{
    int x = 0, y = 0, row = 0, z = 0;
    int n = self->mRows;
    int rows;
    if (n > 0) {
        int color = kFlatNormal;
        do {
            int col = 0;
            int m = self->mCols;
            if (m > 0) {
                do {
                    daPicGate_c::Vertex *v = &self->mCells[row * self->mRows + col];
                    int cols;
                    v->x = x; v->y = y; v->z = z; v->color = color;
                    cols = self->mCols;
                    if (col == cols - 2) x = ((u8)(self->param1 & 0xf) + 1) * kCell;
                    else x += ((u8)(self->param1 & 0xf) + 1) * kCell / (cols - 1);
                    col++;
                } while (col < self->mCols);
            }
            rows = self->mRows;
            x = 0;
            if (row == rows - 2) y = ((u8)((self->param1 >> 4) & 0xf) + 1) * kCell;
            else y += ((u8)((self->param1 >> 4) & 0xf) + 1) * kCell / (rows - 1);
            row++;
        } while (row < rows);
    }
    self->PlaceCorners();
    {
        int width = ((u8)(self->param1 & 0xf) + 1) * kCell;
        int height = ((u8)((self->param1 >> 4) & 0xf) + 1) * kCell;
        self->BeginWave(width / 2, height / 2, 0);
    }
}
}

// @symbol func_ov080_0212677c
/* State 0 render. A dead wave draws the flat quad; otherwise a strip per column. */
extern "C" {
void func_ov080_0212677c(daPicGate_c *self)
{
    int tmp[12];
    int i, j, n;
    int z = 0;

    if (self->mWaveTimer == 0) {
        self->DrawFlat();
        return;
    }

    G3_MTX_PUSH = z;
    MulMat4x3Mat4x3(self->mMtx, data_0209b3ec.m, tmp);
    G3_MTX_MODE = kMtxPosVec;
    LoadMtx43(tmp);
    G3_MTX_MODE = kMtxPos;
    LoadMtx43(tmp);

    G3_LIGHT_VECTOR = 0xe0000000;
    G3_LIGHT_COLOR = 0xc0007fff;
    self->LoadMaterial();

    G3_MTX_SCALE = kScale;
    G3_MTX_SCALE = kScale;
    G3_MTX_SCALE = kScale;

    n = (int)self->mCols;
    i = z; /* zero the column before subtracting, so the sub consumes n */
    n = n - 1;
    if (n > 0) {
        do {
            G3_BEGIN = kTriStrip;
            j = z;
            if ((int)self->mRows > 0) {
                do {
                    daPicGate_c::Vertex *base = self->mCells;
                    int rows = (int)self->mRows;
                    daPicGate_c::Vertex *v1 = &base[i + j * rows];
                    daPicGate_c::Vertex *v2 = &base[(i + 1) + j * rows];
                    int vx, vy, vz;
                    s16 s0, s1, s2;

                    EMIT_VTX(v1);
                    EMIT_VTX(v2);

                    j++;
                } while (j < (int)self->mRows);
            }
            G3_END = z;
            i++;
        } while (i < (int)self->mCols - 1);
    }
    G3_MTX_POP = 1;
}
}

// @symbol func_ov080_021269b8
/* State 0 behavior. Ripples only while mWaveTimer is still counting. */
extern "C" {
void func_ov080_021269b8(daPicGate_c *self)
{
    int i;
    daPicGate_c::Vertex *v;

    self->HitTest();
    if (DecIfAbove0_Short(&self->mWaveTimer) == 0) return;

    for (i = 0; i < self->mNumCells; i++) {
        v = &self->mCells[i];
        v->z = self->RippleHeight(v->dist);
    }

    self->BuildNormals();
    self->FlattenFrame();

    self->mWavePhase = self->mWavePhase + self->mWaveParams->phaseStep;
}
}

// @symbol func_ov080_02126a54
/* State 0 init. Same grid as state 1, plus a texcoord, and no wave yet. */
extern "C" {
void func_ov080_02126a54(daPicGate_c *self)
{
    int x;
    int y;
    int row;
    int n;
    int col;
    x = 0;
    y = 0;
    row = 0;
    if ((int)self->mRows > 0) {
        do {
            col = 0;
            if ((int)self->mCols > 0) {
                do {
                    daPicGate_c::Vertex *v = &self->mCells[row * (int)self->mRows + col];
                    v->x = x;
                    v->y = y;
                    v->z = 0;
                    v->color = kFlatNormal;
                    int du = kTexSpan / ((int)self->mCols - 1);
                    int dv = kTexSpan / ((int)self->mRows - 1);
                    int s = du * col;
                    int t = kTexSpan - dv * row;
                    v->texCoord = (u16)(s16)(s >> 8) | (((u16)(s16)(t >> 8) << 1) << 15);
                    n = (int)self->mCols;
                    if (col == n - 2)
                        x = (u8)(self->param1 & 0xf) * kCell + kCell;
                    else
                        x += ((u8)(self->param1 & 0xf) * kCell + kCell) / (n - 1);
                    col++;
                } while (col < n);
            }
            n = (int)self->mRows;
            x = 0;
            if (row == n - 2)
                y = (u8)((self->param1 >> 4) & 0xf) * kCell + kCell;
            else
                y += ((u8)((self->param1 >> 4) & 0xf) * kCell + kCell) / (n - 1);
            row++;
        } while (row < n);
    }
    self->PlaceCorners();
}
}

// @symbol _ZN11daPicGate_c16CleanupResourcesEv
s32 daPicGate_c::CleanupResources() {
    func_0203cbc0(mCells);
    return 1;
}

// @symbol _ZN11daPicGate_c16OnPendingDestroyEv
void daPicGate_c::OnPendingDestroy() {
}

// @symbol _ZN11daPicGate_c6RenderEv
s32 daPicGate_c::Render() {
    (this->*mState->render)();
    return 1;
}

// @symbol _ZN11daPicGate_c8BehaviorEv
s32 daPicGate_c::Behavior() {
    (this->*mState->behavior)();
    return 1;
}

// @symbol _ZN11daPicGate_c13InitResourcesEv
s32 daPicGate_c::InitResources() {
    unsigned int state = (unsigned char)((param1 >> 0xd) & 3);

    if (state >= 2) {
        mRows = 2;
        mCols = mRows;
        mNumCells = (u16)(mCols * mRows);
    } else {
        unsigned int size = (unsigned char)(param1 & 0xf) + 1;
        switch (size) {
        case 0:
        case 1:
        case 2:
            break;
        case 3:
            mRows = kGridRows[0];
            mCols = mRows;
            break;
        case 4:
            mRows = kGridRows[1];
            mCols = mRows;
            break;
        case 5:
            mRows = kGridRows[2];
            mCols = mRows;
            break;
        case 6:
            mRows = kGridRows[3];
            mCols = mRows;
            break;
        case 7:
            mRows = kGridRows[4];
            mCols = mRows;
            break;
        case 8:
            mRows = kGridRows[5];
            mCols = mRows;
            break;
        case 9:
        case 10:
        case 11:
        case 12:
        case 13:
        case 14:
        case 15:
            break;
        case 16:
            mRows = kGridRows[6];
            mCols = mRows;
            break;
        }
        mNumCells = (u16)(mCols * mRows);
    }

    mCells = (Vertex *)Memory::operator_new2((unsigned)mNumCells * 0x18u);
    mTexRecord = (s32)this->LoadTexture((int)((unsigned char)((param1 >> 8) & 0x1f)));
    mWaveParams = (const daPicGate_c::WaveParams *)&kDefaultWave;
    mState = &kStates[(unsigned char)((param1 >> 0xd) & 3)];
    (this->*mState->init)();

    {
        int width = (int)((unsigned char)(param1 & 0xf) + 1) * kCell;
        int height = (int)((unsigned char)((param1 >> 4) & 0xf) + 1) * kCell;
        SetRanges(this, height / 2, width / 2 + kRangePad, kFar, kFar);
    }
    if ((unsigned char)((param1 >> 8) & 0x1f) == kPicUnclipped) {
        if ((unsigned char)((param1 >> 0xd) & 3) == 1)
            mFlags &= ~kClipEnable;
    }
    if ((unsigned char)((param1 >> 8) & 0x1f) == kPicSlide) {
        if ((unsigned char)((param1 >> 0xd) & 3) == 1) {
            mClosedPosX = mPosX;
            if (kSave[2] & kOpened)
                mPosX += kSlide;
        }
    }
    this->BuildGateMatrix();
    return 1;
}

/* daPicGate_c_classInit is not `new daPicGate_c()`. ~daPicGate_c() is the
 * key function and is defined outside this file, so the new-expression's
 * vptr store reaches the undefined _ZTV11daPicGate_c with addend 8 (the
 * start-of-object spelling); the cartridge stores the address-point label
 * itself, and production isolation refuses an undefined vtable reference
 * with a nonzero addend (measured: the rombuild link control fails on it).
 * The factory keeps the hand-built sequence: operator new(0x1bc), the
 * dActor_c constructor, the vtable store. */
// @symbol daPicGate_c_classInit
extern "C" daPicGate_c *daPicGate_c_classInit()
{
    int *p = (int *)_ZN7fBase_cnwEj(sizeof(daPicGate_c));
    if (p) {
        _ZN8dActor_cC2Ev(p);
        p[0] = (int)_ZTV11daPicGate_c;
    }
    return (daPicGate_c *)p;
}
