//! Copyright © 2024-2025 ChefKiss, licensed under the Thou Shalt Not Profit License version 1.5.
//! See LICENSE for details.

#pragma once
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOTimerEventSource.h>

class ForgedInvariantMain {
    _Atomic(bool) systemAwake;
    _Atomic(bool) synchronising;
    _Atomic(bool) synchronised;
    _Atomic(int) threadsEngaged;
    _Atomic(UInt64) targetTSC;
    bool supportsTscAdjust {false};
    bool lockTSCFreqUsingHWCR {false};
    int threadCount {0};
    int targetThread {0};
    mach_vm_address_t orgXcpmUrgency {0};
    mach_vm_address_t orgTracePoint {0};
    mach_vm_address_t orgClockGetCalendarMicrotime {0};
    IOTimerEventSource *syncTimer {nullptr};

    static void resetTscAdjust(void *);
    void lockTscFreqIfPossible();
    static void setTscValue(void *);

    void syncTsc();
    static void syncTscAction(OSObject *owner, IOTimerEventSource *timer);

    static void wrapXcpmUrgency(int urgency, UInt64 rtPeriod, UInt64 rtDeadline);
    static void wrapTracePoint(void *that, UInt8 point);
    static void wrapClockGetCalendarMicrotime(clock_sec_t *secs, clock_usec_t *microsecs);

    void processPatcher(KernelPatcher &patcher);

    public:
    static ForgedInvariantMain &singleton();

    void init();
};
