// Copyright © 2024-2025 ChefKiss, licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <Headers/kern_patcher.hpp>

class TSCSyncer {
    _Atomic(bool) systemAwake;
    _Atomic(bool) synchronising;
    _Atomic(bool) synchronised;
    _Atomic(int) threadsEngaged;
    _Atomic(UInt64) targetTSC;

    union {
        struct {
            UInt8 tscAdjust : 1;
            UInt8 amd17h : 1;
            UInt8 _rsvd : 6;
        };
        UInt8 raw {0};
    } caps {};

    int threadCount {0};
    mach_vm_address_t orgXcpmUrgency {0};
    mach_vm_address_t orgTracePoint {0};
    mach_vm_address_t orgClockGetCalendarMicrotime {0};

    static void resetAdjust(void *);
    void lockFreq();
    static void setTscValue(void *);

    static void wrapXcpmUrgency(int urgency, UInt64 rtPeriod, UInt64 rtDeadline);
    static void wrapTracePoint(void *that, UInt8 point);
    static void wrapClockGetCalendarMicrotime(clock_sec_t *secs, clock_usec_t *microsecs);

    void processPatcher(KernelPatcher &patcher);

    public:
    static TSCSyncer &singleton();

    void init();
    void sync(bool timer = false);
    bool periodicSyncRequired() const;
};
