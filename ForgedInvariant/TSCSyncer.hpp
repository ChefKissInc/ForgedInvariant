// Copyright © 2024-2025 ChefKiss, licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOTimerEventSource.h>

class TSCForger {
    atomic_bool systemAwake = ATOMIC_VAR_INIT(true);
    atomic_bool synchronising = ATOMIC_VAR_INIT(false);
    atomic_bool synchronised = ATOMIC_VAR_INIT(false);
    atomic_int threadsEngaged = ATOMIC_VAR_INIT(0);
    atomic_ullong targetTSC = ATOMIC_VAR_INIT(0);

    union {
        struct {
            bool tscAdjust : 1;
            bool amd17h : 1;
            UInt8 _rsvd : 6;
        };
        UInt8 raw {0};
    } caps {};

    int threadCount {0};
    mach_vm_address_t orgXcpmUrgency {0};
    mach_vm_address_t orgTracePoint {0};
    mach_vm_address_t orgClockGetCalendarMicrotime {0};
    IOTimerEventSource *timer {nullptr};

    static void resetAdjust(void *);
    void lockFreq();
    static void sync(void *);

    static void wrapXcpmUrgency(int urgency, UInt64 rtPeriod, UInt64 rtDeadline);
    static void wrapTracePoint(void *that, UInt8 point);
    static void wrapClockGetCalendarMicrotime(clock_sec_t *secs, clock_usec_t *microsecs);

    void processPatcher(KernelPatcher &patcher);

    void startTimer();
    void stopTimer();

    static void timerAction(OSObject *owner, IOTimerEventSource *timer);

    void syncAll(bool timer = false);

    public:
    static TSCForger &singleton();

    void init();
};
