//cpp
/* Timer.cpp -- arm9 system stopwatch: one 64-bit tick count plus a running
 * flag. mTime is the start tick while running and the frozen elapsed count
 * while stopped. Covers 0x02019584..0x0201964c; GetSoundMode ends the Sound
 * TU immediately above and the power-management system functions begin
 * immediately below, so this class is the whole TU.
 *
 * `#pragma defer_codegen off` makes mwccarm emit .text in source order, which
 * is the ROM's order here. Do not reorder.
 */
#include "Timer.h"
#include "decl_common.h"

#pragma defer_codegen off

// @symbol _ZN5Timer7GetTimeEv
s64 Timer::GetTime()
{
    if (!mIsRunning)
        return mTime;
    return func_02059650() - mTime;
}

// @symbol _ZN5Timer9StopTimerEv
void Timer::StopTimer()
{
    if (!mIsRunning)
        return;
    mIsRunning = 0;
    mTime = func_02059650() - mTime;
}

// @symbol _ZN5Timer10StartTimerEv
void Timer::StartTimer()
{
    mIsRunning = 1;
    mTime = func_02059650() - mTime;
}

// @symbol _ZN5Timer10ResetTimerEv
void Timer::ResetTimer()
{
    mIsRunning = 0;
    mTime = 0;
}
