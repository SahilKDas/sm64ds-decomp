//cpp
/* daObjPowerUpItem_c -- the power flower: a pickup that gives the player who
 * touches it a character-specific power-up (see func_ov002_020b979c).
 *
 * ROM evidence: _ZTS18daObjPowerUpItem_c is the cartridge type name; the tree's
 * earlier coined spelling was PowerFlower (same vtable, ov002 0x02109800).
 * 19 functions, .text 0x020b9148..0x020b9e64.
 *
 * The out-of-line destructor is the key function, so this TU emits _ZTV/_ZTI/
 * _ZTS. Under `#pragma defer_codegen off` it comes out D1 (0x020b9148), D0
 * (0x020b9198), then a D2 the cartridge has no home for; the same pragma lays
 * .text down in source order, so this file is ROM-ascending. The last
 * function is the registry factory daObjPowerUpItem_c_classInit
 * (0x020b9e0c), `new daObjPowerUpItem_c()`.
 *
 * common.h comes before the class header so the flat Matrix4x3 stands. The
 * two matrix copies were matched against that spelling; Matrix4x3.t is not
 * used. The class header still precedes decl_common.h.
 *
 * Known limits:
 *  - The flower runs a three-state machine (mState 0..2). The per-state
 *    enter/update pairs live in the pointer-to-member table
 *    data_ov002_021097bc, which is zero in the overlay image and filled at run
 *    time. Which helper belongs to which state is inferred from the
 *    transitions between them (94c4 enters state 1, 92c4 enters state 2), not
 *    read from the table.
 *  - The helpers are still free extern "C" functions taking the flower; the
 *    ones declared in decl_common.h keep their `char *` parameter.
 *  - Several bodies keep `(long long)` and `(int)ptr + off` spellings and
 *    mangled `_ZN` callee names from the byte-matching recovery.
 */

#pragma defer_codegen off

#include "common.h"
#include "daObjPowerUpItem_c.h"
#include "SharedFilePtr.h"
#include "SaveData.h"
#include "fBase_c.h"
#include "dCc_c.h"
#include "decl_SaveData.h"
#include "decl_common.h"
#include "dBgCh_Gnd.h"
#include "Player.h"

struct Vector3_16f;
struct Callback;
typedef struct { int x, y, z; } V3;
struct RG { char a[0x14]; int detect[15]; };

/* One row of the state table: enter (pmf[0]) and per-frame update (pmf[1]). */
struct FlowerDispatch;
typedef void (FlowerDispatch::*FlowerPMF)();
struct FlowerEntry { FlowerPMF pmf[2]; int extra; };
extern FlowerEntry data_ov002_021097bc[];
struct FlowerDispatch { char pad[0x3c0]; int idx; };

/* Values of mState, by the transitions between the helpers below. */
enum {
    FLOWER_STATE_LAUNCHED = 0, /* falls (func_ov002_020b94c4) until it lands; Render draws mCloseModel */
    FLOWER_STATE_OPENING = 1,  /* pop animation (func_ov002_020b92c4); mOpenModel */
    FLOWER_STATE_RESTING = 2   /* waits for a player (func_ov002_020b91fc); mOpenModel */
};

/* The life timer never runs for a flower with this param1 (see
 * func_ov002_020b91fc). InitResources starts such a flower resting only when
 * the closest player is Luigi and SaveData::HasPlayerLostCap is false;
 * otherwise it returns 0. */
#define FLOWER_PARAM_PERSISTENT 0xffff

/* Actor IDs from symbols/actor_debug_names.tsv. */
enum {
    ACTOR_PLAYER = 0xbf,
    ACTOR_MONKEY_THIEF = 0x10b,
    ACTOR_MONKEY_STAR = 0x10c
};

/* A Player's param1, as the Init* calls it selects in func_ov002_020b979c
 * imply (InitVanishLuigi, InitMetalWario, InitFireYoshi). */
enum {
    CHARACTER_MARIO = 0,
    CHARACTER_LUIGI = 1,
    CHARACTER_WARIO = 2,
    CHARACTER_YOSHI = 3
};

int ApproachLinear(int&, int, int);
namespace cstd { int fdiv(int, int); }

extern "C" {
extern u8 DecIfAbove0_Byte(u8* p);
extern void _ZN7fBase_c18MarkForDestructionEv(void* p);
extern void* _ZN8Particle6System3NewEjj5Fix12IiES2_S2_PK11Vector3_16fPNS_8CallbackE(
    u32 id, u32 a, int x, int y, int z, const struct Vector3_16f* rot, struct Callback* cb);
extern signed short data_02082214[];
extern void _ZN8Particle6System9NewSimpleEj5Fix12IiES2_S2_(unsigned int, int, int, int);
extern void func_02012694(unsigned int id, const Vector3 *v);
extern void *data_0209f318;
extern void func_0203568c(int *p, int v);
extern void func_02035684(int *p, int v);
extern void dBgCh_Actr_UpdateContinuous_Veneer(void* p);
extern int _ZNK10dBgCh_Actr12TouchesWaterEv(void* self);
extern void *_ZN9dBgCh_GndC1Ev(dBgCh_Gnd*);
extern void _ZN9dBgCh_Gnd12SetObjAndPosERK7Vector3P8dActor_c(dBgCh_Gnd*, const Vector3*, void*);
extern int _ZN9dBgCh_Gnd10DetectClsnEv(struct RG*);
extern void _ZN9dBgCh_GndD1Ev(dBgCh_Gnd*);
extern void _ZN10dBgCh_Actr18StopDetectingWaterEv(void* self);
extern int _ZNK10dBgCh_Actr10IsOnGroundEv(void* self);
extern int func_0200fccc(char* s, int r1);
extern int _ZN6Player15IsCollectingCapEv(void* p);
extern void _ZN6Player16InitWingFeathersEb(void* p, int b);
extern void _ZN6Player16InitBalloonMarioEv(void* p);
extern void _ZN6Player14InitMetalWarioEv(void* p);
extern void _ZN6Player15InitVanishLuigiEv(void* p);
extern void _ZN6Player13InitFireYoshiEv(void* p);
extern int _ZN8dActor_c19DropShadowRadHeightER11ShadowModelR9Matrix4x35Fix12IiES5_j(
    char* self, ShadowModel* sm, struct Matrix4x3* m, int fix, int t, u32 f);
extern void Matrix4x3_FromRotationY(void* m, int angle);
extern void *gPFlowerCloseModelFile[];
extern void *gPFlowerOpenModelFile[];
extern int _ZN9ModelBase7SetFileEP8BMD_Fileii(void *self, void *f, int a, int b);
extern int _ZN11ShadowModel12InitCylinderEv(void *self);
extern void _ZN7dCcAc_c4InitEP8dActor_c5Fix12IiES3_jj(
    void *self, void *act, Fix12i a, Fix12i b, unsigned int c2, unsigned int d);
extern void _ZN10dBgCh_Actr4InitEP8dActor_c5Fix12IiES3_P10Vector3_16S5_(
    void *self, void *act, Fix12i a, Fix12i b, void *d, void *e);
extern void _ZN10dBgCh_Actr19StartDetectingWaterEv(void *self);
}

// @symbol _ZN18daObjPowerUpItem_cD1Ev
// @symbol _ZN18daObjPowerUpItem_cD0Ev
daObjPowerUpItem_c::~daObjPowerUpItem_c()
{
}

/* Update for a resting flower. Unless its param1 is FLOWER_PARAM_PERSISTENT it
 * counts mLifeTimer down and, once that reaches zero, marks itself for
 * destruction (not while the 0x20000 / 0x40000 yoshi-mouth bits are set).
 * Every frame it keeps the particle effect 0x104 following 0x82000 above the
 * flower. */
// @symbol func_ov002_020b91fc
extern "C" void func_ov002_020b91fc(daObjPowerUpItem_c *item)
{
    int flags;
    V3 pos;

    do {
        if (item->param1 == FLOWER_PARAM_PERSISTENT) break;
        if (DecIfAbove0_Byte(&item->mLifeTimer) != 0) break;
        flags = item->mFlags;
        if ((int)((flags & 0x40000) != 0) != 0) break;
        if ((int)((flags & 0x20000) != 0) != 0) break;
        _ZN7fBase_c18MarkForDestructionEv(item);
    } while (0);

    {
        int z = item->mPosZ;
        int x = item->mPosX;
        int y = item->mPosY + 0x82000;
        ((int*)&pos)[0] = x;
        ((int*)&pos)[1] = y;
        ((int*)&pos)[2] = z;
        item->mEffectHandle = (u32)_ZN8Particle6System3NewEjj5Fix12IiES2_S2_PK11Vector3_16fPNS_8CallbackE(
            item->mEffectHandle, 0x104,
            ((int*)&pos)[0], ((int*)&pos)[1], ((int*)&pos)[2],
            0, 0);
    }
}

/* Forgets the particle effect handle. */
// @symbol func_ov002_020b92b8
extern "C" void func_ov002_020b92b8(daObjPowerUpItem_c *item)
{
    item->mEffectHandle = 0;
}

/* Pop animation: while mWobbleTimer counts down, mScaleX and mScaleY follow the
 * table pair selected by mWobbleAngle (scaled by the fraction of the timer
 * left) and the angle advances 0x2000 a frame. When the timer is spent,
 * mScaleY eases back to 0xfa0 and, once there, the flower enters
 * FLOWER_STATE_RESTING. */
// @symbol func_ov002_020b92c4
extern "C" void func_ov002_020b92c4(daObjPowerUpItem_c *item)
{
    if (DecIfAbove0_Byte(&item->mWobbleTimer)) {
        int a = (int)item->mWobbleTimer << 12;
        int fdivResult = cstd::fdiv(a, 0x1c000);
        unsigned short hw = item->mWobbleAngle;
        int idx = hw >> 4;
        signed short odd = data_02082214[idx * 2 + 1];
        int t1 = (int)(((long long)odd * fdivResult + 0x800) >> 12);
        int u1 = (int)(((long long)t1 * 0x332 + 0x800) >> 12);
        int v1 = u1 + 0xffa;
        int w1 = (int)(((long long)v1 * 0xfa0 + 0x800) >> 12);
        item->mScaleY = w1;
        signed short even = data_02082214[idx * 2];
        int t2 = (int)(((long long)even * fdivResult + 0x800) >> 12);
        int u2 = (int)(((long long)t2 * 0x332 + 0x800) >> 12);
        int v2 = u2 + 0xffa;
        int w2 = (int)(((long long)v2 * 0xfa0 + 0x800) >> 12);
        item->mScaleX = w2;
        // materialized base for halfword at offset >= 0x100
        short* p = (short*)(((int)item + 0x3c8));
        *p = *p + 0x2000;
    } else {
        int ret = ApproachLinear(item->mScaleY, 0xfa0, 0x199);
        if (ret) {
            func_ov002_020b9704((char*)item, FLOWER_STATE_RESTING);
        }
    }
}

/* Enter action that starts the pop: puffs particle 0x102 above the flower,
 * plays sound 0x7d, drops the effect handle, takes mAngleY from the halfword at
 * data_0209f318 + 0x17c and starts mWobbleTimer at 0x1b. */
// @symbol func_ov002_020b9450
extern "C" void func_ov002_020b9450(char *self)
{
    daObjPowerUpItem_c *item = (daObjPowerUpItem_c *)self;
    Vector3 pos;
    int x = item->mPosX;
    int y = item->mPosY + 0x82000;
    int z = item->mPosZ;
    ((int *)&pos)[0] = x;
    ((int *)&pos)[1] = y;
    ((int *)&pos)[2] = z;
    _ZN8Particle6System9NewSimpleEj5Fix12IiES2_S2_(
        0x102, ((int *)&pos)[0], ((int *)&pos)[1], ((int *)&pos)[2]);
    func_02012694(0x7d, (const Vector3 *)&item->mCamSpacePosX);
    item->mEffectHandle = 0;
    item->mAngleY = *(short *)((char *)data_0209f318 + 0x17c);
    item->mWobbleTimer = 0x1b;
}

/* Falling update: emits particle 0x103, turns the flower about Y by 0x250 plus
 * an amount that grows with its vertical speed (towards the speed's sign), and
 * moves it with the mesh collision. Touching water either dissolves it in a
 * puff (when the ground probe under it is missing or more than 0x64000 away) or
 * stops the water detection. Landing calls func_0200fccc(item, 1) and enters
 * FLOWER_STATE_OPENING. */
// @symbol func_ov002_020b94c4
extern "C" void func_ov002_020b94c4(daObjPowerUpItem_c *item)
{
    struct RG rg;
    Vector3 pos;
    int a;
    int mag;
    int y;
    s16 delta;
    s16 X;
    int gy;
    int diff;

    item->mEffectHandle = (u32)_ZN8Particle6System3NewEjj5Fix12IiES2_S2_PK11Vector3_16fPNS_8CallbackE(
        item->mEffectHandle, 0x103, item->mPosX, item->mPosY, item->mPosZ, 0, 0);

    a = item->mVertSpeed;
    mag = (a < 0) ? -a : a;
    y = (int)(((s64)mag * 0x120000 + 0x800) >> 12);
    X = (s16)(y / 4096);
    delta = 0x250;
    if (X > 0)
        delta += X;
    if (a > 0)
        item->mAngleY += delta;
    else
        item->mAngleY -= delta;

    func_0203568c((int*)&item->mWithMeshClsn, 0x3c000);
    func_02035684((int*)&item->mWithMeshClsn, 0x3c000);
    item->UpdatePos((dCc_c*)&item->mdCcAc_c);
    dBgCh_Actr_UpdateContinuous_Veneer((void*)&item->mWithMeshClsn);

    if (_ZNK10dBgCh_Actr12TouchesWaterEv((void*)&item->mWithMeshClsn)) {
        pos.x = item->mPosX;
        pos.y = item->mPosY;
        pos.z = item->mPosZ;
        _ZN9dBgCh_GndC1Ev((dBgCh_Gnd*)&rg);
        _ZN9dBgCh_Gnd12SetObjAndPosERK7Vector3P8dActor_c((dBgCh_Gnd*)&rg, &pos, 0);
        if (_ZN9dBgCh_Gnd10DetectClsnEv(&rg)) {
            gy = rg.detect[12];
            pos.y = gy;
            diff = item->mPosY - gy;
            if (diff < 0)
                diff = -diff;
            if (diff > 0x64000) {
                item->SmallPoofDust();
                _ZN7fBase_c18MarkForDestructionEv(item);
                _ZN9dBgCh_GndD1Ev((dBgCh_Gnd*)&rg);
                return;
            }
            _ZN10dBgCh_Actr18StopDetectingWaterEv((void*)&item->mWithMeshClsn);
        } else {
            item->SmallPoofDust();
            _ZN7fBase_c18MarkForDestructionEv(item);
            _ZN9dBgCh_GndD1Ev((dBgCh_Gnd*)&rg);
            return;
        }
        _ZN9dBgCh_GndD1Ev((dBgCh_Gnd*)&rg);
        return;
    }

    if (!_ZNK10dBgCh_Actr10IsOnGroundEv((void*)&item->mWithMeshClsn))
        return;
    func_0200fccc((char*)item, 1);
    func_ov002_020b9704((char*)item, FLOWER_STATE_OPENING);
}

/* Enter action for the launch: plays sound 0x7c, drops the effect handle and
 * sets gravity (-0x668), terminal velocity (-0xf000) and vertical speed
 * (0xd000). */
// @symbol func_ov002_020b96c0
extern "C" void func_ov002_020b96c0(daObjPowerUpItem_c *item)
{
    func_02012694(0x7c, (const Vector3*)&item->mCamSpacePosX);
    item->mEffectHandle = 0;
    item->mVertAccel = 0xfffff998;
    item->mTerminalVelocity = -0xf000;
    item->mVertSpeed = 0xd000;
}

/* Switches to state `i` and runs that state's enter action. */
// @symbol func_ov002_020b9704
extern "C" void func_ov002_020b9704(char *raw, int i) {
  FlowerDispatch *c = (FlowerDispatch *)raw;
  c->idx = i;
  int j = c->idx;
  (c->*data_ov002_021097bc[j].pmf[0])();
}

/* Runs the current state's per-frame update. */
// @symbol func_ov002_020b9750
extern "C" void func_ov002_020b9750(char *raw) {
  FlowerDispatch *c = (FlowerDispatch *)raw;
  int j = c->idx;
  (c->*data_ov002_021097bc[j].pmf[1])();
}

/* Pickup. Looks up the actor whose id sits in the flower's collider
 * (mdCcAc_c.otherOwner) and, if it is a Player that is not collecting a cap,
 * not holding a MONKEY_THIEF and (as Yoshi) not holding a MONKEY_STAR in its
 * mouth, gives it the power-up for its character and destroys the flower:
 * Mario gets wing feathers when this flower's param1 is 1 and a balloon
 * otherwise, Luigi vanishes, Wario turns to metal, Yoshi breathes fire. With
 * the 0x20000 yoshi-mouth bit set on the flower it only resets mLifeTimer to
 * 0x64. */
// @symbol func_ov002_020b979c
extern "C" void func_ov002_020b979c(char* self) {
    daObjPowerUpItem_c *item = (daObjPowerUpItem_c *)self;
    Player* player;
    u32 id = item->mdCcAc_c.otherOwner;
    if (id == 0) return;

    player = (Player*)dActor_c::FindWithID(id);
    if (player == 0) return;

    {
        int b = (int)(player->actorID == ACTOR_PLAYER);
        if (b == 0) return;
    }

    if (_ZN6Player15IsCollectingCapEv(player) != 0) return;

    {
        dActor_c* held = *(dActor_c**)&player->mHeldObj;
        int t = (int)(held != 0);
        if (t != 0) {
            int b = (int)(held->actorID == ACTOR_MONKEY_THIEF);
            if (b != 0) return;
        }
    }

    {
        u32 flags = item->mFlags;
        int t = (int)((flags & 0x20000) != 0);
        if (t != 0) {
            item->mLifeTimer = 0x64;
            return;
        }
    }

    switch (player->param1) {
    case CHARACTER_MARIO:
        if (item->param1 == 1) {
            _ZN6Player16InitWingFeathersEb(player, 1);
        } else {
            _ZN6Player16InitBalloonMarioEv(player);
        }
        _ZN7fBase_c18MarkForDestructionEv(self);
        return;
    case CHARACTER_WARIO:
        _ZN6Player14InitMetalWarioEv(player);
        _ZN7fBase_c18MarkForDestructionEv(self);
        return;
    case CHARACTER_LUIGI:
        _ZN6Player15InitVanishLuigiEv(player);
        _ZN7fBase_c18MarkForDestructionEv(self);
        return;
    case CHARACTER_YOSHI:
        if (*(dActor_c**)&player->mObjInMouth != 0) {
            dActor_c* inMouth = *(dActor_c**)&player->mObjInMouth;
            if (inMouth->actorID == ACTOR_MONKEY_STAR) return;
        }
        _ZN6Player13InitFireYoshiEv(player);
        _ZN7fBase_c18MarkForDestructionEv(self);
        return;
    }
}

/* Shadow: copies mOpenModel's matrix into mShadowMat, puts its Y at the ground
 * height (mGroundY >> 3) and drops the shadow. In state 0 the shadow radius
 * shrinks with the flower's height above the ground (0x64000 down to a floor of
 * 0x3c000); otherwise it is 0x78000. Returns 1 without doing anything while the
 * 0x40000 yoshi-mouth bit is set. */
// @symbol func_ov002_020b993c
extern "C" int func_ov002_020b993c(char* self)
{
    daObjPowerUpItem_c *item = (daObjPowerUpItem_c *)self;
    int r3;
    int b = (int)((item->mFlags & 0x40000) != 0);
    if (b != 0) return b;
    *(struct Matrix4x3*)&item->mShadowMat = *(struct Matrix4x3*)&item->mOpenModel.mat4x3;
    item->mShadowMat.m[10] = item->mGroundY >> 3;
    r3 = 0x78000;
    if (item->mState == FLOWER_STATE_LAUNCHED) {
        int d = item->mPosY - item->mGroundY;
        if (d <= 0x1000) d = 0x1000;
        r3 = 0x64000 - (int)(((s64)d * 0x180 + 0x800) >> 12);
        if (r3 < 0x3c000) r3 = 0x3c000;
    }
    return _ZN8dActor_c19DropShadowRadHeightER11ShadowModelR9Matrix4x35Fix12IiES5_j(self, &item->mShadowModel, &item->mShadowMat, r3, 0x3c000, 0xf);
}

/* Model matrices: rotates mOpenModel's matrix by mAngleY, sets its translation
 * to the position >> 3 and copies it to mCloseModel. */
// @symbol func_ov002_020b9a1c
extern "C" void func_ov002_020b9a1c(char* self){
  daObjPowerUpItem_c *item = (daObjPowerUpItem_c *)self;
  Matrix4x3_FromRotationY(&item->mOpenModel.mat4x3, item->mAngleY);
  item->mOpenModel.mat4x3.m[9] = item->mPosX >> 3;
  item->mOpenModel.mat4x3.m[10] = item->mPosY >> 3;
  item->mOpenModel.mat4x3.m[11] = item->mPosZ >> 3;
  *(struct Matrix4x3*)&item->mCloseModel.mat4x3 = *(struct Matrix4x3*)&item->mOpenModel.mat4x3;
}

// @symbol _ZN18daObjPowerUpItem_c16CleanupResourcesEv
s32 daObjPowerUpItem_c::CleanupResources()
{
    ((SharedFilePtr *)gPFlowerCloseModelFile)->Release();
    ((SharedFilePtr *)gPFlowerOpenModelFile)->Release();
    return 1;
}

// @symbol _ZN18daObjPowerUpItem_c6RenderEv
int daObjPowerUpItem_c::Render()
{
  int f = (int)((mFlags & 0x40000) != 0);
  if (f != 0) return 1;
  /* blinks while mLifeTimer is below 0x2d */
  unsigned char st = mLifeTimer;
  if (st < 0x2d && (st & 1)) return 1;
  switch (mState) {
  case FLOWER_STATE_LAUNCHED: mCloseModel.Render((const Vector3 *)&mScaleX); break;
  case FLOWER_STATE_OPENING: mOpenModel.Render((const Vector3 *)&mScaleX); break;
  case FLOWER_STATE_RESTING: mOpenModel.Render((const Vector3 *)&mScaleX); break;
  }
  return 1;
}

// @symbol _ZN18daObjPowerUpItem_c8BehaviorEv
int daObjPowerUpItem_c::Behavior()
{
    int b = (int)((mFlags & 0x40000) != 0);
    if (b != 0) return 1;
    mScaleX = 0xfa0;
    mScaleY = 0xfa0;
    mScaleZ = 0xfa0;
    func_ov002_020b9750(((char*)this));
    func_ov002_020b979c(((char*)this));
    func_ov002_020b9a1c(((char*)this));
    func_ov002_020b993c(((char*)this));
    ((dCc_c *)&mdCcAc_c)->Clear();
    ((dCc_c *)&mdCcAc_c)->Update();
    if (SaveData::HasPlayerLostCap()) {
        ((dActor_c *)(((char*)this)))->SmallPoofDust();
        ((fBase_c *)(((char*)this)))->MarkForDestruction();
    }
    return 1;
}

// @symbol _ZN18daObjPowerUpItem_c13InitResourcesEv
int daObjPowerUpItem_c::InitResources()
{
    struct Vector3 pos;
    short *angp;

    Model::LoadFile(*(SharedFilePtr *)gPFlowerCloseModelFile);
    Model::LoadFile(*(SharedFilePtr *)gPFlowerOpenModelFile);
    if (_ZN9ModelBase7SetFileEP8BMD_Fileii(((char *)this) + 0x124, gPFlowerOpenModelFile[1], 1, -1) == 0)
        return 0;
    if (_ZN9ModelBase7SetFileEP8BMD_Fileii(((char *)this) + 0xd4, gPFlowerCloseModelFile[1], 1, -1) == 0)
        return 0;
    if (_ZN11ShadowModel12InitCylinderEv((char *)&mShadowModel) == 0)
        return 0;

    mVertAccel = -0x668;
    mTerminalVelocity = -0xf000;
    func_ov002_020b9a1c(((char *)this));

    mScaleX = 0xfa0;
    mScaleY = 0xfa0;
    mScaleZ = 0xfa0;

    _ZN7dCcAc_c4InitEP8dActor_c5Fix12IiES3_jj(((char *)this) + 0x1cc, ((char *)this), 0x32000, 0x64000, 0x800002, 0x8000);
    _ZN10dBgCh_Actr4InitEP8dActor_c5Fix12IiES3_P10Vector3_16S5_(((char *)this) + 0x200, ((char *)this), 0x3c000, 0x3c000, 0, 0);
    _ZN10dBgCh_Actr19StartDetectingWaterEv((char *)&mWithMeshClsn);

    /* ground probe: a ray starting 0x14000 above the flower */
    pos.x = mPosX;
    pos.y = mPosY;
    pos.z = mPosZ;
    pos.y += 0x14000;
    dBgCh_Gnd ground;
    ground.SetObjAndPos(pos, 0);
    mGroundY = pos.y;
    if (ground.DetectClsn())
        mGroundY = ground.clsnY;
    mLifeTimer = 0xb4;

    if (param1 == FLOWER_PARAM_PERSISTENT) {
        if (*(int *)((char *)((dActor_c *)this)->ClosestPlayer() + 8) == CHARACTER_LUIGI && _ZN8SaveData16HasPlayerLostCapEv() == 0) {
            func_ov002_020b9704(((char *)this), FLOWER_STATE_RESTING);
        } else {
            return 0;
        }
    } else {
        func_ov002_020b9704(((char *)this), FLOWER_STATE_LAUNCHED);
    }
    angp = (short *)(int)((char *)&mAngleY);
    *angp = *angp - 0x4000;
    return 1;
}

// @symbol _ZN18daObjPowerUpItem_c13OnYoshiTryEatEv
s32 daObjPowerUpItem_c::OnYoshiTryEat()
{
    return 5;
}

/* Reconstructed source-style name: SM64DS proves daObjPowerUpItem_c through
 * RTTI, allocation size, vtable identity, and the POWER_UP_ITEM registry
 * profile; later EAD lineage supplies classInit. Exact original spelling is
 * not preserved. Historical alias: PowerFlower_Spawn. */
// @symbol daObjPowerUpItem_c_classInit
extern "C" daObjPowerUpItem_c *daObjPowerUpItem_c_classInit()
{
    return new daObjPowerUpItem_c();
}
