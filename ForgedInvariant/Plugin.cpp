// Copyright © 2024-2025 ChefKiss, licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include "Plugin.hpp"
#include "TSCSyncer.hpp"
#include <Headers/kern_version.hpp>

static constexpr UInt32 PERIODIC_SYNC_INTERVAL = 5000;

OSDefineMetaClassAndStructors(PRODUCT_NAME, IOService);

PRODUCT_NAME *ADDPR(selfInstance) = nullptr;

IOService *PRODUCT_NAME::probe(IOService *provider, SInt32 *score) {
    if (!ADDPR(startSuccess)) { return nullptr; }

    ADDPR(selfInstance) = this;
    this->setProperty("VersionInfo", kextVersion);
    return IOService::probe(provider, score);
}

bool PRODUCT_NAME::start(IOService *provider) {
    ADDPR(selfInstance) = this;

    if (!IOService::start(provider)) {
        SYSLOG("init", "super::start failed");
        return false;
    }

    // If we have no way to lock the rate of the TSC, then we must sync it periodically.
    if (checkKernelArgument("-FIPeriodic") || TSCSyncer::singleton().periodicSyncRequired()) {
        DBGLOG("init", "Will have to sync periodically.");

        this->timer = IOTimerEventSource::timerEventSource(this, timerAction);
        if (this->timer == nullptr) {
            this->stop(provider);
            return false;
        }
        this->startTimer();
    }

    TSCSyncer::singleton().sync();

    return true;
}

void PRODUCT_NAME::stop(IOService *provider) {
    ADDPR(selfInstance) = nullptr;
    this->stopTimer();
    IOService::stop(provider);
}

void PRODUCT_NAME::free() {
    OSSafeReleaseNULL(this->timer);
    IOService::free();
}

void PRODUCT_NAME::startTimer() {
    if (this->timer == nullptr) { return; }
    this->timer->enable();
    this->timer->setTimeoutMS(PERIODIC_SYNC_INTERVAL);
}

void PRODUCT_NAME::stopTimer() {
    if (this->timer == nullptr) { return; }
    this->timer->cancelTimeout();
    this->timer->disable();
}

void PRODUCT_NAME::timerAction(OSObject *, IOTimerEventSource *sender) {
    TSCSyncer::singleton().sync(true);
    sender->setTimeoutMS(PERIODIC_SYNC_INTERVAL);
}

static const char *bootargOff = "-FIOff";
static const char *bootargDebug = "-FIDebug";
static const char *bootargBeta = "-FIBeta";

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    &bootargOff,
    1,
    &bootargDebug,
    1,
    &bootargBeta,
    1,
    KernelVersion::SnowLeopard,
    KernelVersion::Tahoe,
    []() { TSCSyncer::singleton().init(); },
};
