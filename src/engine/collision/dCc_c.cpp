//cpp
/* The cylinder-collision root (arm9 .text 0x02014aa8..0x020150e8): dCc_c, the
 * intrusive-list node that dCcPos_c, dCcAc_c and dCcAcPos_c derive from.
 * Process() walks the global list whose head is data_0209cee8, finds pairs
 * whose flags interlock, and writes per-node pushback; the two file-static
 * helpers resolve a flag-8 hit's owner to the player actor and notify it.
 * mwccarm emits .text in reverse source order, so the definitions below run
 * ROM-descending.
 *
 * ~dCc_c() is the key function: mwcc emits D2, D0 and D1 from the one
 * definition and the cartridge keeps all three (0x02015058 / 0x0201507c /
 * 0x020150a8). The complete-object ctor variant C1 is the homeless copy --
 * the ROM keeps only C2 at 0x020150cc, which every derived constructor
 * enters through the base chain. The vtable/RTTI records land in .data as
 * compiler-emitted output externalized to the canonical bytes
 * (_ZTS5dCc_c 0x0208e658, _ZTI5dCc_c 0x0208e660, _ZTV5dCc_c 0x0208e6ec).
 */
#include "dCc_c.h"

struct Player;
struct Arg;

extern "C" {
/* local extern: the collider-list head. decl_common.h's copy is `void*`; the
   real type is the node pointer every method here reads. */
extern dCc_c *data_0209cee8;
extern Fix12i Vec3_HorzLen(const Vector3 *v);
/* local extern: file-static pair below -- the head-link helpers only this
   TU calls. */
void func_02014f44(u32 id, dCc_c *clsn);
/* local extern: no header declares it; the definition lives in Player.cpp
   (extern "C", takes its collider param as its file-local `Arg` scaffold --
   arg->m2() is vtable slot 2, our GetPos, and it tests the flags word at
   +0x18). */
int func_ov002_020caf98(struct Player *player, struct Arg *clsn);
}

namespace cstd { int fdiv(int a, int b); }

// @symbol _ZN5dCc_cC2Ev
/* C2, the base-subobject constructor. The vptr store is the compiler's; the
 * body is only what the ROM's constructor actually writes -- both intrusive
 * list links zeroed, an unlinked node. */
dCc_c::dCc_c()
{
    prev = 0;
    next = 0;
}

// @symbol _ZN5dCc_cD1Ev
/* One definition emits D2, D0 and D1: the destructor body's only obligation
   is the intrusive-list unlink, and D0 follows it with the class-local
   operator delete (Memory::operator_delete2, see the header). */
dCc_c::~dCc_c()
{
    Unlink();
}

// @symbol _ZN5dCc_c4InitE5Fix12IiES1_jj
/* Stays a mangled free definition: the real signature carries Fix12<int> and
   wall 6az (notes/mwccarm-codegen.md) homes class-typed by-value parameters.
   The declaration in dCc_c.h is the real one and callers may use it. */
extern "C" void _ZN5dCc_c4InitE5Fix12IiES1_jj(struct dCc_c *self, int a, int b, unsigned d, unsigned e) {
  *(int*)&self->radius=a;
  *(int*)&self->height=b;
  *(int*)&self->flags=d;
  *(int*)&self->vulnFlags=e;
}

// @symbol _ZN5dCc_c5ClearEv
void dCc_c::Clear()
{
    pushback.x = 0;
    pushback.y = 0;
    pushback.z = 0;
    otherOwner = 0;
    hitFlags = 0;
}

// @symbol _ZN5dCc_c6UpdateEv
/* Link this node in at the head of the active list. The mirror of the unlink
   that the destructors run; see include/dCc_c.h. */
void dCc_c::Update()
{
    if (flags & 1) return;
    next = data_0209cee8;
    if (data_0209cee8) data_0209cee8->prev = this;
    data_0209cee8 = this;
}

// @symbol _ZN5dCc_c6UnlinkEv
/* Removes this collision node from the global intrusive list. The member role
 * is proven by the prev/next rewiring; the original spelling `Unlink` is an
 * evidence-bounded reconstruction. */
void dCc_c::Unlink()
{
    if (prev)
        prev->next = next;
    else if (data_0209cee8 == this)
        data_0209cee8 = next;

    if (next)
        next->prev = prev;

    prev = 0;
    next = 0;
}

// @symbol func_02014f5c
#pragma cplusplus off
/* The two helpers are compiled by the C front end -- their shard files are
   .c and the bodies below are byte-identical only under it. `self` is a
   collider node the whole way through: Process passes a dCc_c, which the
   player notifier reads as its Arg (the flags word at +0x18 and the m2()
   position). */
struct dActor_c {
    void* vtable;
    u32 uniqueID;
    u32 param1;
    u16 actorID;
};

extern struct dActor_c* _ZN8dActor_c10FindWithIDEj(u32 id);

#define PLAYER_ACTOR_ID 0xbf

/* find the hitting owner by actor ID and, when it is the player, notify him */
void func_02014f5c(struct dCc_c* self, u32 id) {
    struct dActor_c* player;
    u32 isPlayer;

    player = _ZN8dActor_c10FindWithIDEj(id);
    if (!player) return;

    isPlayer = (player->actorID == PLAYER_ACTOR_ID) ? 1 : 0;
    if (!isPlayer) return;

    func_ov002_020caf98((struct Player*)player, (struct Arg*)self);
}

// @symbol func_02014f44
void func_02014f44(unsigned int id, struct dCc_c* self) {
    func_02014f5c(self, id);
}
#pragma cplusplus on

// @symbol _ZN5dCc_c7ProcessEv
void dCc_c::Process()
{
    u32 owner0;
    Fix12i vertDist;
    Vector3* pos0;
    dCc_c* other;
    Fix12i dist;
    Fix12i overlap;
    u32 sharedFlags;
    Fix12i minDist;
    Vector3 diff;

    if (data_0209cee8 == 0)
        return;

    minDist = 0x1000;

    do
    {
        pos0 = &data_0209cee8->GetPos();
        owner0 = data_0209cee8->GetOwnerID();

        for (other = data_0209cee8->next; other != 0; other = other->next)
        {
            sharedFlags = (data_0209cee8->flags & other->vulnFlags) | (data_0209cee8->vulnFlags & other->flags);
            if (sharedFlags == 0)
                continue;
            if (owner0 != 0 && owner0 == other->GetOwnerID())
                continue;

            Vector3& pos1 = other->GetPos();
            diff.y = pos1.y - pos0->y;
            if (diff.y < 0)
                vertDist = other->height + diff.y;
            else
                vertDist = data_0209cee8->height - diff.y;
            if (vertDist <= 0)
                continue;

            diff.x = pos1.x - pos0->x;
            diff.z = pos1.z - pos0->z;
            dist = Vec3_HorzLen(&diff);
            if (dist == 0)
            {
                dist = minDist;
                diff.x = dist;
            }

            overlap = data_0209cee8->radius + other->radius - dist;
            if (overlap <= 0)
                continue;

            data_0209cee8->hitFlags = other->flags;
            other->hitFlags = data_0209cee8->flags;
            data_0209cee8->otherOwner = other->GetOwnerID();
            other->otherOwner = data_0209cee8->GetOwnerID();

            if (sharedFlags & 0x4000000)
            {
                u32* pf1 = (u32*)((char*)other + 0x20);
                u32* pf0 = (u32*)((char*)data_0209cee8 + 0x20);
                *pf0 |= 0x8000000;
                overlap -= 0x50000;
                *pf1 |= 0x8000000;
                if (overlap < 0)
                    continue;
            }

            {
                u32 f0 = data_0209cee8->flags;
                if (f0 & 2)
                    continue;
                u32 f1 = other->flags;
                if (f1 & 2)
                    continue;

                if (f0 & 4)
                {
                    if (f1 & 4)
                    {
                        if (diff.y < 0)
                        {
                            Fix12i h = vertDist >> 1;
                            data_0209cee8->pushback.y = h;
                            other->pushback.y = -h;
                        }
                        else
                        {
                            Fix12i h = vertDist >> 1;
                            data_0209cee8->pushback.y = -h;
                            other->pushback.y = h;
                        }
                        other->pushback.x = 0;
                        other->pushback.z = 0;
                    }
                    else
                    {
                        if (diff.y < 0)
                            other->pushback.y = -vertDist;
                        else
                            other->pushback.y = vertDist;
                        if (data_0209cee8->flags & 8)
                        {
                            Fix12i r = data_0209cee8->radius;
                            Fix12i t = overlap - (r - (((r << 1) + r) >> 3));
                            if (t < 0)
                                overlap = 0;
                            else
                                overlap = cstd::fdiv(t, dist);
                            func_02014f44(data_0209cee8->otherOwner, data_0209cee8);
                        }
                        else
                            overlap = cstd::fdiv(overlap, dist);
                        other->pushback.x = (Fix12i)(((long long)diff.x * overlap + 0x800) >> 12);
                        other->pushback.z = (Fix12i)(((long long)diff.z * overlap + 0x800) >> 12);
                    }
                    data_0209cee8->pushback.x = 0;
                    data_0209cee8->pushback.z = 0;
                }
                else if (f1 & 4)
                {
                    if (diff.y < 0)
                        data_0209cee8->pushback.y = vertDist;
                    else
                        data_0209cee8->pushback.y = -vertDist;
                    if (other->flags & 8)
                    {
                        Fix12i r = data_0209cee8->radius;
                        Fix12i t = overlap - (r - (((r << 1) + r) >> 3));
                        if (t < 0)
                            overlap = 0;
                        else
                            overlap = cstd::fdiv(t, dist);
                        func_02014f44(other->otherOwner, other);
                    }
                    else
                        overlap = cstd::fdiv(overlap, dist);
                    data_0209cee8->pushback.x = -(Fix12i)(((long long)diff.x * overlap + 0x800) >> 12);
                    data_0209cee8->pushback.z = -(Fix12i)(((long long)diff.z * overlap + 0x800) >> 12);
                    other->pushback.x = 0;
                    other->pushback.z = 0;
                }
                else if (!(f0 & 0x10000000) && !(f1 & 0x10000000))
                {
                    if (diff.y < 0)
                    {
                        Fix12i h = vertDist >> 1;
                        data_0209cee8->pushback.y = h;
                        other->pushback.y = -h;
                    }
                    else
                    {
                        Fix12i h = vertDist >> 1;
                        data_0209cee8->pushback.y = -h;
                        other->pushback.y = h;
                    }
                    Fix12i ratio = cstd::fdiv(overlap, dist) >> 1;
                    data_0209cee8->pushback.x = -(Fix12i)(((long long)diff.x * ratio + 0x800) >> 12);
                    data_0209cee8->pushback.z = -(Fix12i)(((long long)diff.z * ratio + 0x800) >> 12);
                    other->pushback.x = (Fix12i)(((long long)diff.x * ratio + 0x800) >> 12);
                    other->pushback.z = (Fix12i)(((long long)diff.z * ratio + 0x800) >> 12);
                }
            }
        }

        {
            dCc_c* nxt = data_0209cee8->next;
            data_0209cee8->prev = 0;
            data_0209cee8->next = 0;
            data_0209cee8 = nxt;
        }
    } while (data_0209cee8 != 0);
}
