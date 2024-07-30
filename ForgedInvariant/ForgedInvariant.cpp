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
constexpr UInt32 CPUID_AMD_FAMILY_17H = 0x17;
constexpr UInt32 CPUID_INTEL_FAMILY_6H = 6;
constexpr UInt32 CPUID_INTEL_MODEL_PENRYN = 23;
constexpr UInt64 CPUID_FEATURE_HTT = getBit<UInt64>(28);

constexpr UInt32 MSR_TSC = 0x10;
constexpr UInt32 MSR_TSC_ADJUST = 0x3B;
constexpr UInt32 MSR_HWCR = 0xC0010015;
constexpr UInt64 MSR_HWCR_LOCK_TSC_TO_CURR_P0 = getBit<UInt64>(21);

constexpr UInt8 kIOPMTracePointSleepCPUs = 0x18;
constexpr UInt8 kIOPMTracePointWakePlatformActions = 0x22;

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
    // What are you so urgent for, if the clock is not working, huh??
    // Maybe you should've used an actually reliable clock source
    // instead of the number of cycles the CPU has done.
    if (!atomic_load_explicit(&synchronised, memory_order_relaxed)) { return; }
    FunctionCast(wrapXcpmUrgency, callback->orgXcpmUrgency)(urgency, rtPeriod, rtDeadline);
}

void ForgedInvariantMain::wrapTracePoint(void *that, UInt8 point) {
    switch (point) {
        case kIOPMTracePointSleepCPUs: {    // Those CPUs sure like to sleep.
            atomic_store_explicit(&systemAwake, false, memory_order_relaxed);
            atomic_store_explicit(&synchronised, false, memory_order_relaxed);
            break;
        }
        case kIOPMTracePointWakePlatformActions: {    // Oh, now you want to wake up, you lazy turd?
            atomic_store_explicit(&systemAwake, true, memory_order_relaxed);
            callback->syncTsc();
            break;
        }
        default: {    // Don't care. Lol!
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
    UInt32 eax;
    UInt32 ebx;
    UInt32 ecx;
    UInt32 edx;

    SYSLOG("Main", "Copyright (c) 2024 ChefKiss. If you've paid for this, you've been scammed.");
    callback = this;

    // Monterey got a change in the task scheduling requiring the target TSC value
    // to be in synchronise with the first thread and not the last one.
    this->constants.newSyncMethod = getKernelVersion() >= KernelVersion::Monterey;
    // CPUID Leaf 7 Count 0 Bit 1 defines whether a CPU supports TSC_ADJUST, according to the Intel SDM.
    this->constants.resetTscAdjust =
        this->constants.newSyncMethod && CPUInfo::getCpuid(7, 0, nullptr, &ebx) && (ebx & CPUID_LEAF7_TSC_ADJUST) != 0;

    const BaseDeviceInfo &info = BaseDeviceInfo::get();
    switch (info.cpuVendor) {
        case CPUInfo::CpuVendor::Unknown: {
            PANIC("Main", "Who made your CPU? Black Mesa?");
            break;
        }
        case CPUInfo::CpuVendor::AMD: {
            // For AMD, we try to determine the thread count using an AMD-specific CPUID extension.
            if (CPUInfo::getCpuid(0x80000008, 0, nullptr, nullptr, &ecx)) {
                // Amount of threads that the system has are stored in bits 0..8
                this->constants.threadCount = (ecx & 0xFF) + 1;
            } else {
                SYSLOG("Main", "AMD-specific extension not supported...");
            }

            // We must get the family manually on AMD because Acidanthera designs
            // their software with the intent to make the life of AMD CPU users
            // absolute hell and will therefore not fill in the extended CPUID
            // information if the CPU is an AMD one. And yes, the logic is the same.
            if (CPUInfo::getCpuid(1, 0, &eax)) {
                const CPUInfo::CpuVersion *ver = reinterpret_cast<const CPUInfo::CpuVersion *>(&eax);
                const UInt32 family = ver->family == 0xF ? (ver->family + ver->extendedFamily) : ver->family;
                // The specific bit in the HWCR MSR is only available since 17h.
                this->constants.lockTSCFreqUsingHWCR = family >= CPUID_AMD_FAMILY_17H;
            } else {
                SYSLOG("Main", "No CPUID leaf 1? [insert related megamind picture here]");
                this->constants.lockTSCFreqUsingHWCR = false;
                if (this->constants.threadCount == 0) {
                    SYSLOG("Main", "Setting thread count to 1 as both the CPUID leaf 1 and the AMD-specific extension "
                                   "are not present!");
                    this->constants.threadCount = 1;
                }
            }
            break;
        }
        case CPUInfo::CpuVendor::Intel: {
            // On Intel, we try to determine the thread count using MSR_CORE_THREAD_COUNT
            // Which is only available after Penryn, according to the XNU source code.
            // The Intel SDM seems to disagree (?) and says it's available since Haswell-E.
            // Thanks, very cool!
            if (info.cpuFamily > CPUID_INTEL_FAMILY_6H ||
                (info.cpuFamily == CPUID_INTEL_FAMILY_6H && info.cpuModel > CPUID_INTEL_MODEL_PENRYN)) {
                // bits 0..16 of this MSR contain the thread count, according to the Intel SDM.
                this->constants.threadCount = rdmsr64(MSR_CORE_THREAD_COUNT) & 0xFFFF;
            } else {
                SYSLOG("Main", "MSR_CORE_THREAD_COUNT not supported!");
            }
            break;
        }
    }

    // Failed to get the thread count using modern methods,
    // we must get it through CPUID.
    if (this->constants.threadCount == 0) {
        SYSLOG("Main", "Failed to get thread count via modern methods, using CPUID!");

        if (CPUInfo::getCpuid(1, 0, nullptr, &ebx, &ecx, &edx)) {
            const UInt64 features = (static_cast<UInt64>(ecx) << 32) | edx;
            if (features & CPUID_FEATURE_HTT) {
                // If the HTT feature is supported then ebc will contain the
                // maximum APIC ID that's usable at 16..23
                this->constants.threadCount = (ebx >> 16) & 0xFF;
            } else {
                // Damn... that's one ancient CPU huh?
                // Or a buggy VMM
                this->constants.threadCount = 1;
            }

        } else {
            SYSLOG("Main", "No CPUID leaf 1? [insert related megamind picture here]");
            this->constants.threadCount = 1;
        }
    }

    this->constants.targetThread = this->constants.newSyncMethod ? 0 : (this->constants.threadCount - 1);

    DBGLOG("Main", "Synchronisation method: %s.", this->constants.newSyncMethod ? "New" : "Old");
    DBGLOG("Main", "TSC_ADJUST: %s.", this->constants.resetTscAdjust ? "Available" : "Unavailable");
    DBGLOG("Main", "LockTscToCurrentP0: %s.", this->constants.lockTSCFreqUsingHWCR ? "Available" : "Unavailable");
    DBGLOG("Main", "Thread count: %d.", this->constants.threadCount);
    DBGLOG("Main", "Target thread: %d.", this->constants.targetThread);

    lockTscFreqIfPossible();

    lilu.onPatcherLoadForce(
        [](void *user, KernelPatcher &patcher) { static_cast<ForgedInvariantMain *>(user)->processPatcher(patcher); },
        this);
}
