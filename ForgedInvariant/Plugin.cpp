// Copyright © 2024-2025 ChefKiss, licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include "TSCSyncer.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_version.hpp>
#include <Headers/plugin_start.hpp>

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
    []() { TSCForger::singleton().init(); },
};
