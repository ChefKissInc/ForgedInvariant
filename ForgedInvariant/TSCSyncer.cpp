// Copyright © 2024-2025 ChefKiss, licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include "TSCSyncer.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <i386/proc_reg.h>

static constexpr UInt32 PERIODIC_SYNC_INTERVAL = 5000;

static constexpr UInt32 CPUID_LEAF7_TSC_ADJUST = getBit<UInt32>(1);
static constexpr UInt64 CPUID_FEATURE_HTT = getBit<UInt64>(28);

static constexpr UInt32 MSR_TSC = 0x10;
static constexpr UInt32 MSR_TSC_ADJUST = 0x3B;
static constexpr UInt32 MSR_HWCR = 0xC0010015;
static constexpr UInt64 MSR_HWCR_LOCK_TSC_TO_CURR_P0 = getBit<UInt64>(21);

enum {
    kIOPMTracePointSleepCPUs = 0x18,
    kIOPMTracePointWakePlatformActions = 0x22,
};

static TSCForger instance {};

TSCForger &TSCForger::singleton() { return instance; }

void TSCForger::resetAdjust(void *) {
    atomic_fetch_add_explicit(&singleton().threadsEngaged, 1, memory_order_relaxed);
    while (atomic_load_explicit(&singleton().threadsEngaged, memory_order_relaxed) != singleton().threadCount) {}

    wrmsr64(MSR_TSC_ADJUST, 0);
}

void TSCForger::lockFreq() {
    // On AMD Family 17h and newer, we can take advantage of the LockTscToCurrentP0 bit
    // which allows us to lock the frequency of the TSC to the current P0 frequency
    // and prevent it from changing regardless of future changes to it.
    if (this->caps.amd17h) { wrmsr64(MSR_HWCR, rdmsr64(MSR_HWCR) | MSR_HWCR_LOCK_TSC_TO_CURR_P0); }
}

void TSCForger::sync(void *) {
    singleton().lockFreq();

    __c11_atomic_fetch_max(&singleton().targetTSC, rdtsc64(), __ATOMIC_SEQ_CST);

    atomic_fetch_add(&singleton().threadsEngaged, 1);
    while (atomic_load(&singleton().threadsEngaged) != singleton().threadCount) {}

    wrmsr64(MSR_TSC, atomic_load_explicit(&singleton().targetTSC, memory_order_relaxed));
}

void TSCForger::syncAll(bool timer) {
    // Ensure we don't try to synchronise multiple times at once or when the system is sleeping.
    if (!atomic_load(&this->systemAwake) || (!timer && atomic_load(&this->synchronised)) ||
        atomic_exchange(&this->synchronising, true)) {
        return;
    }

    atomic_store(&this->synchronised, false);

    // If TSC_ADJUST is supported, just reset it.
    // Otherwise, synchronise the TSC value itself.
    atomic_store(&this->threadsEngaged, 0);
    if (this->caps.tscAdjust) {
        mp_rendezvous_no_intrs(resetAdjust, nullptr);
    } else {
        atomic_store(&this->targetTSC, 0);
        mp_rendezvous_no_intrs(sync, nullptr);
    }

    atomic_store(&this->synchronising, false);
    atomic_store(&this->synchronised, true);
}

void TSCForger::wrapXcpmUrgency(int urgency, UInt64 rtPeriod, UInt64 rtDeadline) {
    // Maybe you should've used a reliable clock source.
    if (!atomic_load_explicit(&singleton().synchronised, memory_order_relaxed)) { return; }
    FunctionCast(wrapXcpmUrgency, singleton().orgXcpmUrgency)(urgency, rtPeriod, rtDeadline);
}

void TSCForger::wrapTracePoint(void *that, UInt8 point) {
    switch (point) {
        case kIOPMTracePointSleepCPUs: {    // Those CPUs sure like to sleep.
            atomic_store_explicit(&singleton().systemAwake, false, memory_order_relaxed);
            atomic_store_explicit(&singleton().synchronised, false, memory_order_relaxed);
            singleton().stopTimer();
        } break;
        case kIOPMTracePointWakePlatformActions: {    // So now you want to wake up, huh?
            atomic_store_explicit(&singleton().systemAwake, true, memory_order_relaxed);
            singleton().syncAll();
            singleton().startTimer();
        } break;
    }
    FunctionCast(wrapTracePoint, singleton().orgTracePoint)(that, point);
}

void TSCForger::wrapClockGetCalendarMicrotime(clock_sec_t *secs, clock_usec_t *microsecs) {
    singleton().syncAll();
    FunctionCast(wrapClockGetCalendarMicrotime, singleton().orgClockGetCalendarMicrotime)(secs, microsecs);
}

void TSCForger::processPatcher(KernelPatcher &patcher) {
    this->syncAll();

    KernelPatcher::RouteRequest requests[] = {
        {"_xcpm_urgency", wrapXcpmUrgency, this->orgXcpmUrgency},
        {"__ZN14IOPMrootDomain10tracePointEh", wrapTracePoint, this->orgTracePoint},
        {"_clock_get_calendar_microtime", wrapClockGetCalendarMicrotime, this->orgClockGetCalendarMicrotime},
    };
    PANIC_COND(!patcher.routeMultiple(KernelPatcher::KernelID, requests), "TSCSyncer", "Failed to route symbols");
}

void TSCForger::init() {
    UInt32 ebx;
    UInt32 ecx;
    UInt32 edx;

    SYSLOG("TSCSyncer", "|-----------------------------------------------------------------|");
    SYSLOG("TSCSyncer", "| Copyright 2024-2025 ChefKiss.                                   |");
    SYSLOG("TSCSyncer", "| If you've paid for this, you've been scammed. Ask for a refund! |");
    SYSLOG("TSCSyncer", "| Do not support tonymacx86. Support us, we truly care.           |");
    SYSLOG("TSCSyncer", "| Change the world for the better.                                |");
    SYSLOG("TSCSyncer", "|-----------------------------------------------------------------|");

    const BaseDeviceInfo &info = BaseDeviceInfo::get();
    switch (info.cpuVendor) {
        case CPUInfo::CpuVendor::Unknown: {
            SYSLOG("TSCSyncer", "Who made your CPU? Black Mesa?");
            return;
        }
        case CPUInfo::CpuVendor::AMD: {
            // Try to determine the thread count using an AMD-specific CPUID extension.
            if (CPUInfo::getCpuid(0x80000008, 0, nullptr, nullptr, &ecx)) {
                // Last thread index is stored in bits 0..8
                this->threadCount = (ecx & 0xFF) + 1;
            } else {
                DBGLOG("TSCSyncer", "AMD-specific extension not supported...");
            }

            // We must get the family manually on AMD because Acidanthera doesn't care
            // about the quality of their software. And yes, the logic is the same as Intel.
            union {
                CPUInfo::CpuVersion bits;
                uint32_t raw;
            } version;
            if (CPUInfo::getCpuid(1, 0, &version.raw)) {
                const UInt32 family = version.bits.family == 0xF ? (version.bits.family + version.bits.extendedFamily) :
                                                                   version.bits.family;
                // The specific bit in the HWCR MSR is only available since 17h.
                this->caps.amd17h = family >= 0x17;
            } else {
                SYSLOG("TSCSyncer", "No CPUID leaf 1? [insert related megamind picture here]");
                if (this->threadCount == 0) {
                    SYSLOG("TSCSyncer", "Setting thread count to 1 as both CPUID leaf 1 and the AMD-specific extension "
                                        "are not present!");
                    this->threadCount = 1;
                }
            }
        } break;
        case CPUInfo::CpuVendor::Intel: {
            // CPUID Leaf 7 Count 0 Bit 1 defines whether a CPU supports TSC_ADJUST, according to the Intel SDM.
            this->caps.tscAdjust = CPUInfo::getCpuid(7, 0, nullptr, &ebx) && (ebx & CPUID_LEAF7_TSC_ADJUST) != 0;

            // Try to determine the thread count using MSR_CORE_THREAD_COUNT.
            // bits 0..16 of this MSR contain the thread count, according to the Intel SDM.
            // The MSR is only available after Penryn, according to the XNU source code.
            // The Intel SDM seems to disagree (?) and says it's available since Haswell-E.
            // Thanks, very cool!
            if (info.cpuFamily > 6 || (info.cpuFamily == 6 && info.cpuModel > CPUInfo::CPU_MODEL_PENRYN)) {
                this->threadCount = rdmsr64(MSR_CORE_THREAD_COUNT) & 0xFFFF;
            } else {
                SYSLOG("TSCSyncer", "MSR_CORE_THREAD_COUNT not supported!");
            }
        } break;
    }

    if (this->threadCount == 0) {
        DBGLOG("TSCSyncer", "Failed to get thread count via modern methods, using CPUID!");

        if (CPUInfo::getCpuid(1, 0, nullptr, &ebx, &ecx, &edx)) {
            const UInt64 features = (static_cast<UInt64>(ecx) << 32) | edx;
            // If the HTT feature is supported then ebc will contain the
            // maximum APIC ID that's usable at 16..23
            if (features & CPUID_FEATURE_HTT) {
                this->threadCount = (ebx >> 16) & 0xFF;
            } else {
                this->threadCount = 1;    // shit
            }
        } else {
            SYSLOG("TSCSyncer", "No CPUID leaf 1? [insert related megamind picture here]");
            this->threadCount = 1;
        }
    }

    DBGLOG("TSCSyncer", "TSC_ADJUST: %s.", this->caps.tscAdjust ? "Available" : "Unavailable");
    DBGLOG("TSCSyncer", "LockTscToCurrentP0: %s.", this->caps.amd17h ? "Available" : "Unavailable");
    DBGLOG("TSCSyncer", "Thread count: %d.", this->threadCount);

    lilu.onPatcherLoadForce(
        [](void *user, KernelPatcher &patcher) { static_cast<TSCForger *>(user)->processPatcher(patcher); }, this);

    // If we have no way to lock the rate of the TSC, then we must sync it periodically.
    if (checkKernelArgument("-FIPeriodic") || (!this->caps.tscAdjust && !this->caps.amd17h)) {
        DBGLOG("TSCSyncer", "Will have to sync periodically.");

        // timerEventSource fails if inOwner is nullptr so just give it some random OSObject.
        this->timer = IOTimerEventSource::timerEventSource(kOSBooleanTrue, timerAction);
        this->startTimer();
    }
}

void TSCForger::startTimer() {
    if (this->timer == nullptr) { return; }

    this->timer->enable();
    this->timer->setTimeoutMS(PERIODIC_SYNC_INTERVAL);
}

void TSCForger::stopTimer() {
    if (this->timer == nullptr) { return; }

    this->timer->cancelTimeout();
    this->timer->disable();
}

void TSCForger::timerAction(OSObject *, IOTimerEventSource *sender) {
    singleton().syncAll(true);
    sender->setTimeoutMS(PERIODIC_SYNC_INTERVAL);
}
