//! Copyright © 2024 ChefKiss, licensed under the Thou Shalt Not Profit License version 1.5.
//! See LICENSE for details.

#include "ForgedInvariant.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_version.hpp>
#include <Headers/plugin_start.hpp>

static ForgedInvariantMain fi;

static const char *bootargDebug = "-FIDebug";

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    nullptr,
    0,
    &bootargDebug,
    1,
    nullptr,
    0,
    KernelVersion::ElCapitan,
    KernelVersion::Sonoma,
    []() { fi.init(); },
};
