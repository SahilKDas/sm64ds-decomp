#pragma once

#include "types.h"

/* Event bitfield helpers over the single event-flag word at data_0209f34c.
 * `bit` is an index into that word, not a mask.
 */
namespace Event {

s32 ClearBit(u32 bit);
void SetBit(u32 bit);
s32 GetBit(u32 bit);

}

extern s32 data_0209f34c;
