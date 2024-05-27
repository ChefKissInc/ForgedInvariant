//! Copyright © 2024 ChefKiss, licensed under the Thou Shalt Not Profit License version 1.5.
//! See LICENSE for details.

#include "ForgedInvariant.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_cpu.hpp>
#include <Headers/kern_devinfo.hpp>
#include <i386/proc_reg.h>

ForgedInvariantMain *ForgedInvariantMain::callback = nullptr;
_Atomic(bool) ForgedInvariantMain::systemAwake = ATOMIC_VAR_INIT(true);
_Atomic(bool) ForgedInvariantMain::synchronised = ATOMIC_VAR_INIT(false);
_Atomic(int) ForgedInvariantMain::threadsEngaged = ATOMIC_VAR_INIT(0);
_Atomic(UInt64) ForgedInvariantMain::targetTSC = ATOMIC_VAR_INIT(0);

constexpr UInt32 CPUID_LEAF7_TSC_ADJUST = getBit<UInt32>(1);
constexpr UInt32 MSR_TSC = 0x10;
constexpr UInt32 MSR_TSC_ADJUST = 0x3B;
constexpr UInt32 MSR_HWCR = 0xC0010015;
constexpr UInt64 MSR_HWCR_LOCK_TSC_TO_CURR_P0 = getBit<UInt64>(21);

void ForgedInvariantMain::resetTscAdjust(void *) { wrmsr64(MSR_TSC_ADJUST, 0); }

void ForgedInvariantMain::lockTscFreqIfPossible() {
    // On AMD Family 17h and newer, we can take advantage of the LockTscToCurrentP0 bit
    // which allows us to lock the frequency of the TSC to the current P0 frequency
    // and prevent it from changing regardless of future changes to it.
    if (callback->constants.lockTSCFreqUsingHWCR) {
        wrmsr64(MSR_HWCR, rdmsr64(MSR_HWCR) | MSR_HWCR_LOCK_TSC_TO_CURR_P0);
    }
}

void ForgedInvariantMain::setTscValue(void *) {
    lockTscFreqIfPossible();

    // Thread: Hey, I'm here! What did I miss?
    atomic_fetch_add_explicit(&threadsEngaged, 1, memory_order_relaxed);

    // If we are the target thread, store the value for the other threads.
    // Otherwise, wait until the TSC value is set.
    if (cpu_number() == callback->constants.targetThread) {
        atomic_store_explicit(&targetTSC, rdtsc64(), memory_order_relaxed);
    } else {
        while (atomic_load_explicit(&targetTSC, memory_order_relaxed) == 0) {}
    }

    // Barrier: Wait until all threads have reached this point.
    while (atomic_load_explicit(&threadsEngaged, memory_order_relaxed) != callback->constants.threadCount) {}

    // Set the TSC value of all threads to the same exact one.
    wrmsr64(MSR_TSC, atomic_load_explicit(&targetTSC, memory_order_relaxed));
}

void ForgedInvariantMain::syncTsc() {
    atomic_store_explicit(&synchronised, false, memory_order_relaxed);

    // If we are on macOS 12 and newer and TSC_ADJUST is supported, just reset it.
    // Otherwise, we have to synchronise the TSC value itself.
    if (this->constants.resetTscAdjust) {
        mp_rendezvous_no_intrs(resetTscAdjust, nullptr);
    } else {
        atomic_store_explicit(&threadsEngaged, 0, memory_order_relaxed);
        atomic_store_explicit(&targetTSC, 0, memory_order_relaxed);
        mp_rendezvous_no_intrs(setTscValue, nullptr);
    }

    atomic_store_explicit(&synchronised, true, memory_order_relaxed);
}

void ForgedInvariantMain::wrapXcpmUrgency(int urgency, UInt64 rtPeriod, UInt64 rtDeadline) {
    if (!atomic_load_explicit(&synchronised, memory_order_relaxed)) { return; }
    FunctionCast(wrapXcpmUrgency, callback->orgXcpmUrgency)(urgency, rtPeriod, rtDeadline);
}

constexpr UInt8 kIOPMTracePointSleepCPUs = 0x18;
constexpr UInt8 kIOPMTracePointWakePlatformActions = 0x22;

void ForgedInvariantMain::wrapTracePoint(void *that, UInt8 point) {
    switch (point) {
        case kIOPMTracePointSleepCPUs: {
            atomic_store_explicit(&systemAwake, false, memory_order_relaxed);
            atomic_store_explicit(&synchronised, false, memory_order_relaxed);
            break;
        }
        case kIOPMTracePointWakePlatformActions: {
            atomic_store_explicit(&systemAwake, true, memory_order_relaxed);
            callback->syncTsc();
            break;
        }
        default: {
            break;
        }
    }
    FunctionCast(wrapTracePoint, callback->orgTracePoint)(that, point);
}

void ForgedInvariantMain::wrapClockGetCalendarMicrotime(clock_sec_t *secs, clock_usec_t *microsecs) {
    // Don't need to synchronise the TSC if it's already synchronised.
    // Also shouldn't synchronise it if the OS is supposed to be sleeping.
    if (!atomic_load_explicit(&synchronised, memory_order_relaxed) &&
        atomic_load_explicit(&systemAwake, memory_order_relaxed)) {
        callback->syncTsc();
    }
    FunctionCast(wrapClockGetCalendarMicrotime, callback->orgClockGetCalendarMicrotime)(secs, microsecs);
}

void ForgedInvariantMain::processPatcher(KernelPatcher &patcher) {
    KernelPatcher::RouteRequest requests[] = {
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
    // to be in synchronise with the first thread and not the last one.
    this->constants.newSyncMethod = getKernelVersion() >= KernelVersion::Monterey;
    UInt32 reg = 0;
    // CPUID Leaf 7 Count 0 Bit 1 defines whether a CPU supports TSC_ADJUST, according to the Intel SDM.
    this->constants.resetTscAdjust =
        this->constants.newSyncMethod && CPUInfo::getCpuid(7, 0, nullptr, &reg) && (reg & CPUID_LEAF7_TSC_ADJUST) != 0;

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
            reg = 0;
            if (CPUInfo::getCpuid(1, 0, &reg)) {
                CPUInfo::CpuVersion *ver = reinterpret_cast<CPUInfo::CpuVersion *>(&reg);
                UInt32 family = ver->family == 0xF ? ver->family + ver->extendedFamily : ver->family;
                this->constants.lockTSCFreqUsingHWCR = family >= 0x17;
            } else {
                this->constants.lockTSCFreqUsingHWCR = false;
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

    DBGLOG("Main", "Will use new synchronisation method? %s.", this->constants.newSyncMethod ? "Yes" : "No");
    DBGLOG("Main", "Will reset TSC_ADJUST? %s.", this->constants.resetTscAdjust ? "Yes" : "No");
    DBGLOG("Main", "Can use LockTscToCurrentP0? %s.", this->constants.lockTSCFreqUsingHWCR ? "Yes" : "No");
    DBGLOG("Main", "System has %d threads.", this->constants.threadCount);
    DBGLOG("Main", "Will fetch TSC value from thread %d.", this->constants.targetThread);

    lockTscFreqIfPossible();

    lilu.onPatcherLoadForce(
        [](void *user, KernelPatcher &patcher) { static_cast<ForgedInvariantMain *>(user)->processPatcher(patcher); },
        this);
}
