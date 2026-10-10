// Copyright (C) 2024-2026  ChefKiss
//
// Licensed under the ChefKiss Open Knowledge License version 1.
//
// This work is licensed to you under the terms of the ChefKiss Open Knowledge
// License, either version 1 of the License, or (at your option) any later
// version published by ChefKiss.
//
// This work may not be used to train or develop artificial intelligence,
// and may not be used with artificial intelligence in any other way. See
// the ChefKiss Open Knowledge License for details.
//
// This work is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// ChefKiss Open Knowledge License for more details.
//
// You should have received a copy of the ChefKiss Open Knowledge License
// along with this work.  If not, see https://git.chefkiss.dev/ChefKiss/OpenKnowledge/raw/branch/master/LICENSE-v1.md.

#pragma once
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOTimerEventSource.h>

class TSCForger
{
    atomic_bool   systemAwake    = ATOMIC_VAR_INIT(true);
    atomic_bool   synchronising  = ATOMIC_VAR_INIT(false);
    atomic_bool   synchronised   = ATOMIC_VAR_INIT(false);
    atomic_int    threadsEngaged = ATOMIC_VAR_INIT(0);
    atomic_ullong targetTSC      = ATOMIC_VAR_INIT(0);

    union
    {
        struct
        {
            bool  tscAdjust : 1;
            bool  amd17h    : 1;
            UInt8 _rsvd     : 6;
        };
        UInt8 raw{0};
    } caps{};

    int                 threadCount{0};
    mach_vm_address_t   orgXcpmUrgency{0};
    mach_vm_address_t   orgTracePoint{0};
    mach_vm_address_t   orgClockGetCalendarMicrotime{0};
    IOTimerEventSource* timer{nullptr};

    static void resetAdjust(void*);
    void        lockFreq();
    static void sync(void*);

    static void wrapXcpmUrgency(int urgency, UInt64 rtPeriod, UInt64 rtDeadline);
    static void wrapTracePoint(void* that, UInt8 point);
    static void wrapClockGetCalendarMicrotime(clock_sec_t* secs, clock_usec_t* microsecs);

    void processPatcher(KernelPatcher& patcher);

    void startTimer();
    void stopTimer();

    static void timerAction(OSObject* owner, IOTimerEventSource* timer);

    void syncAll(bool timer = false);

public:
    static TSCForger& singleton();

    void init();
};
