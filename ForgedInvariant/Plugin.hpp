// Copyright © 2025 ChefKiss, licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#pragma once
#include <Headers/kern_api.hpp>
#include <Headers/plugin_start.hpp>
#include <IOKit/IOTimerEventSource.h>

class EXPORT PRODUCT_NAME : public IOService {
    OSDeclareDefaultStructors(PRODUCT_NAME);

    IOTimerEventSource *timer {nullptr};

    public:
    IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;

    void startTimer();
    void stopTimer();

    static void timerAction(OSObject *owner, IOTimerEventSource *timer);
};

extern PRODUCT_NAME *ADDPR(selfInstance);
