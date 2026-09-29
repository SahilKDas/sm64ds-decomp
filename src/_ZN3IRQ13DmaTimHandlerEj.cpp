//cpp
#include "IRQ.h"
// @symbol _ZN3IRQ13DmaTimHandlerEj
#include "types.h"
extern "C" {
extern IRQ::HandlerEntry data_020a60c4[];
extern u16 data_02099fd4[];
extern u32 data_023c0000[];
}
typedef void (*DmaTimerCallback)(void *);

inline DmaTimerCallback GetCallback(IRQ::HandlerEntry *entries, u32 index)
{
  return (DmaTimerCallback)entries[index].handler;
}

namespace IRQ {

void DmaTimHandler(u32 index)
{
  u16 irqBit = data_02099fd4[index];
  u32 irqMask = 1u << irqBit;
  DmaTimerCallback callback = GetCallback(data_020a60c4, index);
  data_020a60c4[index].handler = 0;
  if (callback != 0)
  {
    callback((void *)data_020a60c4[index].argument);
  }
  *(volatile u32 *)((int)data_023c0000 + 0x3ff8) |= irqMask;
  if (data_020a60c4[index].enabled != 0)
  {
    return;
  }
  IRQ::DisableIRQs(irqMask);
}

}
