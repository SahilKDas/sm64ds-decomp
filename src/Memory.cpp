//cpp
#include "Memory.h"
#include "Heap.h"

// Deferred codegen emits the definitions in reverse order, so the Heap*-taking
// forms come first in the file; each earlier definition is also the declaration
// the convenience overloads call.

namespace Memory {

// @symbol _ZN6Memory8AllocateEjiP4Heap
void *Allocate(u32 size, int align, Heap *heap)
{
    if (!heap)
        heap = defaultHeapPtr;
    return heap->Allocate(size, align);
}

// @symbol _ZN6Memory10DeallocateEPvP4Heap
void Deallocate(void *ptr, Heap *heap)
{
    if (!heap)
        heap = defaultHeapPtr;
    heap->Deallocate(ptr);
}

// @symbol _ZN6Memory8AllocateEji
void *Allocate(u32 size, int align)
{
    return Allocate(size, align, 0);
}

// @symbol _ZN6Memory8AllocateEj
void *Allocate(u32 size)
{
    return Allocate(size, 4, 0);
}

// @symbol _ZN6Memory10DeallocateEPv
void Deallocate(void *ptr)
{
    Deallocate(ptr, 0);
}

}
