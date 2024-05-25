//! Copyright © 2024 ChefKiss, licensed under the Thou Shalt Not Profit License version 1.5.
//! See LICENSE for details.

#include "ForgedInvariant.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_cpu.hpp>
#include <Headers/kern_devinfo.hpp>
#include <i386/proc_reg.h>

ForgedInvariantMain *ForgedInvariantMain::callback = nullptr;
_Atomic(bool) ForgedInvariantMain::syncCompleted = ATOMIC_VAR_INIT(false);
_Atomic(int) ForgedInvariantMain::threadsEngaged = ATOMIC_VAR_INIT(0);
_Atomic(UInt64) ForgedInvariantMain::targetTSC = ATOMIC_VAR_INIT(0);

constexpr UInt32 MSR_IA32_TSC = 0x10;
constexpr UInt32 MSR_IA32_TSC_ADJUST = 0x3B;

void ForgedInvariantMain::resetTscAdjust(void *) { wrmsr64(MSR_IA32_TSC_ADJUST, 0); }

void ForgedInvariantMain::setTscValue(void *) {
    // Barrier: Wait until all threads have reached this point.
    atomic_fetch_add_explicit(&threadsEngaged, 1, memory_order_relaxed);
    while (atomic_load_explicit(&threadsEngaged, memory_order_relaxed) != callback->constants.threadCount) {}

    // If we are the target thread, store the value for the other threads.
    if (cpu_number() == callback->constants.targetThread) {
        atomic_store_explicit(&targetTSC, rdtsc64(), memory_order_relaxed);
    } else {
        // Otherwise, wait until the TSC value is set, then set the TSC MSR with the new value.
        while (atomic_load_explicit(&targetTSC, memory_order_relaxed) == 0) {}
        wrmsr64(MSR_IA32_TSC, atomic_load_explicit(&targetTSC, memory_order_relaxed));
    }
}

void ForgedInvariantMain::syncTsc() {
    // If we are on macOS 12 and newer and TSC_ADJUST is supported, just reset it.
    atomic_store_explicit(&syncCompleted, false, memory_order_relaxed);
    if (this->constants.newSyncMethod && this->constants.supportsTscAdjust) {
        mp_rendezvous_no_intrs(resetTscAdjust, nullptr);
    } else {
        // Otherwise, we have to sync the TSC value itself.
        atomic_store_explicit(&threadsEngaged, 0, memory_order_relaxed);
        atomic_store_explicit(&targetTSC, 0, memory_order_relaxed);
        mp_rendezvous_no_intrs(setTscValue, nullptr);
    }
    atomic_store_explicit(&syncCompleted, true, memory_order_relaxed);
}

void ForgedInvariantMain::wrapXcpmUrgency(int urgency, UInt64 rtPeriod, UInt64 rtDeadline) {
    if (!atomic_load_explicit(&syncCompleted, memory_order_relaxed)) { return; }
    FunctionCast(wrapXcpmUrgency, callback->orgXcpmUrgency)(urgency, rtPeriod, rtDeadline);
}

constexpr UInt8 kIOPMTracePointWakeCPUs = 0x23;

void ForgedInvariantMain::wrapTracePoint(void *that, UInt8 point) {
    if (point == kIOPMTracePointWakeCPUs) { callback->syncTsc(); }
    FunctionCast(wrapTracePoint, callback->orgTracePoint)(that, point);
}

void ForgedInvariantMain::wrapClockGetCalendarMicrotime(clock_sec_t *secs, clock_usec_t *microsecs) {
    if (!atomic_load_explicit(&syncCompleted, memory_order_relaxed)) { callback->syncTsc(); }
    FunctionCast(wrapClockGetCalendarMicrotime, callback->orgClockGetCalendarMicrotime)(secs, microsecs);
}

void ForgedInvariantMain::processPatcher(KernelPatcher &patcher) {
    KernelPatcher::RouteRequest requests[] {
        {"_xcpm_urgency", wrapXcpmUrgency, this->orgXcpmUrgency},
        {"__ZN14IOPMrootDomain10tracePointEh", wrapTracePoint, this->orgTracePoint},
        {"_clock_get_calendar_microtime", wrapClockGetCalendarMicrotime, this->orgClockGetCalendarMicrotime},
    };

    PANIC_COND(!patcher.routeMultiple(KernelPatcher::KernelID, requests), "Main", "Failed to route symbols");
}

void ForgedInvariantMain::init() {
    SYSLOG("Main", "Copyright (c) 2024 ChefKiss. If you've paid for this, you've been scammed.");
    callback = this;

    // Monterey got a change in the task scheduling requiring the target TSC value
    // to be in sync with the first thread and not the last one.
    this->constants.newSyncMethod = getKernelVersion() >= KernelVersion::Monterey;
    UInt32 reg = 0;
    // CPUID Leaf 7 Count 0 Bit 1 defines whether a CPU supports TSC_ADJUST, according to the Intel SDM.
    this->constants.supportsTscAdjust = CPUInfo::getCpuid(7, 0, nullptr, &reg) && (reg & getBit<UInt32>(1)) != 0;

    switch (BaseDeviceInfo::get().cpuVendor) {
        case CPUInfo::CpuVendor::Unknown: {
            PANIC("Main", "CPU Vendor is unknown.");
        }
        case CPUInfo::CpuVendor::AMD: {
            // For AMD, we need to determine the thread count using an AMD-specific CPUID extension
            reg = 0;
            if (CPUInfo::getCpuid(0x80000008, 0, nullptr, nullptr, &reg)) {
                // Amount of threads that the system has are stored in bits 0..8
                this->constants.threadCount = (reg & 0xFF) + 1;
            } else {
                SYSLOG("Main", "AMD-specific extension not supported, assuming the CPU is so old that it only has "
                               "1 thread...");
                this->constants.threadCount = 1;
            }
            break;
        }
        case CPUInfo::CpuVendor::Intel: {
            // bits 0..16 of this MSR contain the thread count, according to the Intel SDM.
            this->constants.threadCount = rdmsr64(MSR_CORE_THREAD_COUNT) & 0xFFFF;
            break;
        }
    }

    this->constants.targetThread = this->constants.newSyncMethod ? 0 : (this->constants.threadCount - 1);

    DBGLOG("Main", "New sync method: %s.", this->constants.newSyncMethod ? "yes" : "no");
    DBGLOG("Main", "Supports TSC_ADJUST: %s.", this->constants.supportsTscAdjust ? "yes" : "no");
    DBGLOG("Main", "Thread count: %d.", this->constants.threadCount);
    DBGLOG("Main", "Target thread: %d.", this->constants.targetThread);

    lilu.onPatcherLoadForce(
        [](void *user, KernelPatcher &patcher) { static_cast<ForgedInvariantMain *>(user)->processPatcher(patcher); },
        this);
}
