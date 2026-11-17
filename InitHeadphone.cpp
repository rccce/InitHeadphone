/*
 * InitHeadphone.kext
 *
 * Re-initialises the headphone amplifier found in some Clevo laptops
 * (SMBus address 0x73) at boot and after every wake, through VoodooSMBus.
 * Without this, the headphone jack of these laptops stays silent after sleep.
 *
 * The register sequence is the "init" command (effect 1) of init-headphone
 * by Unrud: https://github.com/Unrud/init-headphone (GPL-3.0-or-later).
 * This file is distributed under the same licence.
 *
 * How it works:
 *  1. InitHeadphoneLoader attaches to the VoodooSMBus controller and asks it,
 *     through callPlatformFunction("VoodooSMBusCompanionDevice"), for a device
 *     nub at 0x73. This is the same request VoodooPS2 makes for the trackpad.
 *  2. VoodooSMBus registers that nub; InitHeadphone matches it (Info.plist:
 *     "VoodooSMBUS Slave Device Address" = 115) and owns the amplifier.
 *  3. InitHeadphone writes the init sequence 3 s after boot and 1 s after
 *     every full wake (kIOMessageSystemHasPoweredOn), retrying up to 5 times.
 *     All transfers go through VoodooSMBus, so they never collide with the
 *     trackpad on the same bus.
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>

#define IH_LOG(fmt, ...) IOLog("InitHeadphone: " fmt "\n", ##__VA_ARGS__)

// The two VoodooSMBus 3.0 (de.leo-labs.VoodooSMBus) nub methods used here.
// Both are exported by VoodooSMBus; VoodooRMI's RMISMBus uses the same API.
class VoodooSMBusDeviceNub : public IOService {
    OSDeclareDefaultStructors(VoodooSMBusDeviceNub);
public:
    IOReturn readByteData(UInt8 command);               // 0..255, or a negative IOReturn
    IOReturn writeByteData(UInt8 command, UInt8 value);
};

namespace {
constexpr UInt8  kAmpAddress   = 0x73;
constexpr UInt32 kEffectCount  = 7;
constexpr UInt32 kBootDelayMS  = 3000;
constexpr UInt32 kWakeDelayMS  = 1000;
constexpr UInt32 kRetryDelayMS = 1000;
constexpr UInt32 kMaxAttempts  = 5;

// init-headphone DATA_EFFECTS: values for registers 0x04, 0x05, 0x07, 0x08, 0x09
const UInt8 kEffectRegs[5] = {0x04, 0x05, 0x07, 0x08, 0x09};
const UInt8 kEffects[kEffectCount][5] = {
    {0x11, 0x02, 0x22, 0x82, 0x22},   // effect0
    {0xee, 0x03, 0x40, 0x84, 0xff},   // effect1, used by "init-headphone init"
    {0xaa, 0x23, 0x40, 0x84, 0x00},   // effect2
    {0xaa, 0x22, 0x33, 0x84, 0x00},   // effect3
    {0x88, 0x03, 0x23, 0x82, 0x22},   // effect4
    {0xaa, 0x23, 0x41, 0x84, 0x00},   // effect5
    {0xaa, 0x02, 0x43, 0x82, 0x00},   // effect6
};
const UInt8 kPrologRegs[2] = {0x04, 0x09};
}

// ---------------------------------------------------------------------------
// InitHeadphoneLoader: asks VoodooSMBus for the nub at 0x73
// ---------------------------------------------------------------------------

class InitHeadphoneLoader : public IOService {
    OSDeclareDefaultStructors(InitHeadphoneLoader);
public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(InitHeadphoneLoader, IOService);

bool InitHeadphoneLoader::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;

    // VoodooSMBus checks that param1 is an IOService and param2 an OSDictionary,
    // creates the nub (param3 = address), registers it synchronously and keeps
    // it only if a driver attached to it.
    OSDictionary *props = OSDictionary::withCapacity(1);
    if (props == nullptr)
        return false;
    IOReturn ret = provider->callPlatformFunction("VoodooSMBusCompanionDevice", false,
        this, props, reinterpret_cast<void *>(static_cast<uintptr_t>(kAmpAddress)), nullptr);
    props->release();

    if (ret != kIOReturnSuccess) {
        IH_LOG("VoodooSMBus did not add a device at 0x%02x (0x%08x)", kAmpAddress, static_cast<unsigned>(ret));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// InitHeadphone: drives the amplifier through the nub
// ---------------------------------------------------------------------------

class InitHeadphone : public IOService {
    OSDeclareDefaultStructors(InitHeadphone);
public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;

private:
    VoodooSMBusDeviceNub *nub {nullptr};
    IOWorkLoop *workLoop {nullptr};
    IOTimerEventSource *timer {nullptr};
    IONotifier *powerNotifier {nullptr};
    UInt32 effect {1};
    UInt32 attempt {0};
    UInt32 initCount {0};
    bool awake {true};
    const char *trigger {"boot"};

    void releaseResources();
    IOReturn initAmplifier(int *step);
    void runInit();
    static void timerAction(OSObject *owner, IOTimerEventSource *sender);
    static IOReturn setAwakeAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);
    static IOReturn powerMessage(void *target, void *refCon, UInt32 messageType,
                                 IOService *provider, void *messageArgument, vm_size_t argSize);
};

OSDefineMetaClassAndStructors(InitHeadphone, IOService);

bool InitHeadphone::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;

    nub = OSDynamicCast(VoodooSMBusDeviceNub, provider);
    if (nub == nullptr) {
        IH_LOG("provider is not a VoodooSMBus device nub");
        return false;
    }

    // Optional "Effect" (0 to 6) in the Info.plist personality; default 1 as in init-headphone.
    OSNumber *number = OSDynamicCast(OSNumber, getProperty("Effect"));
    if (number != nullptr && number->unsigned32BitValue() < kEffectCount)
        effect = number->unsigned32BitValue();
    setProperty("Effect", effect, 32);
    setProperty("InitCount", initCount, 32);

    workLoop = IOWorkLoop::workLoop();
    if (workLoop != nullptr)
        timer = IOTimerEventSource::timerEventSource(this, &InitHeadphone::timerAction);
    if (timer == nullptr || workLoop->addEventSource(timer) != kIOReturnSuccess) {
        IH_LOG("could not create the work loop");
        releaseResources();
        return false;
    }

    IOService *rootDomain = getPMRootDomain();
    if (rootDomain != nullptr)
        powerNotifier = rootDomain->registerInterest(gIOGeneralInterest, &InitHeadphone::powerMessage, this, nullptr);
    if (powerNotifier == nullptr) {
        IH_LOG("could not register for sleep and wake messages");
        releaseResources();
        return false;
    }

    IH_LOG("amplifier at 0x%02x, effect %u", kAmpAddress, effect);
    timer->setTimeoutMS(kBootDelayMS);
    return true;
}

void InitHeadphone::stop(IOService *provider)
{
    releaseResources();
    IOService::stop(provider);
}

void InitHeadphone::free()
{
    releaseResources();
    IOService::free();
}

void InitHeadphone::releaseResources()
{
    if (powerNotifier != nullptr) {
        powerNotifier->remove();
        powerNotifier = nullptr;
    }
    if (timer != nullptr) {
        timer->cancelTimeout();
        if (workLoop != nullptr)
            workLoop->removeEventSource(timer);   // waits for a running timer action
        timer->release();
        timer = nullptr;
    }
    if (workLoop != nullptr) {
        workLoop->release();
        workLoop = nullptr;
    }
    nub = nullptr;
}

// init-headphone: write_prolog(), then DATA_DISABLE_OUTPUT + DATA_EFFECTS[effect] + DATA_ENABLE_OUTPUT.
// Runs on our work loop only.
IOReturn InitHeadphone::initAmplifier(int *step)
{
    IOReturn ret;

    *step = 1;
    ret = nub->writeByteData(0x0a, 0x41);
    if (ret != kIOReturnSuccess)
        return ret;

    for (int i = 0; i < 2; i++) {
        *step = 2 + i;
        IOReturn value = nub->readByteData(kPrologRegs[i]);
        if (value < 0)
            return value;
        ret = nub->writeByteData(kPrologRegs[i], static_cast<UInt8>(value));
        if (ret != kIOReturnSuccess)
            return ret;
    }

    *step = 4;
    ret = nub->writeByteData(0x00, 0x86);                 // output off
    if (ret != kIOReturnSuccess)
        return ret;

    for (int i = 0; i < 5; i++) {
        *step = 5 + i;
        ret = nub->writeByteData(kEffectRegs[i], kEffects[effect][i]);
        if (ret != kIOReturnSuccess)
            return ret;
    }

    *step = 10;
    ret = nub->writeByteData(0x00, 0x82);                 // output on
    if (ret != kIOReturnSuccess)
        return ret;

    *step = 0;
    return kIOReturnSuccess;
}

void InitHeadphone::runInit()
{
    if (!awake || nub == nullptr)
        return;

    int step = 0;
    IOReturn ret = initAmplifier(&step);
    if (ret == kIOReturnSuccess) {
        initCount++;
        setProperty("InitCount", initCount, 32);
        setProperty("LastError", 0ULL, 32);
        IH_LOG("amplifier initialised after %s (effect %u, attempt %u)", trigger, effect, attempt + 1);
        attempt = 0;
        return;
    }

    setProperty("LastError", static_cast<UInt32>(ret), 32);
    attempt++;
    if (attempt < kMaxAttempts) {
        IH_LOG("step %d failed after %s (0x%08x), retry %u of %u",
               step, trigger, static_cast<unsigned>(ret), attempt, kMaxAttempts - 1);
        timer->setTimeoutMS(kRetryDelayMS);
    } else {
        IH_LOG("giving up after %u attempts (step %d, 0x%08x)", attempt, step, static_cast<unsigned>(ret));
        attempt = 0;
    }
}

void InitHeadphone::timerAction(OSObject *owner, IOTimerEventSource *sender)
{
    (void)sender;
    InitHeadphone *self = OSDynamicCast(InitHeadphone, owner);
    if (self != nullptr)
        self->runInit();
}

// Runs on our work loop, so it waits for an init that is in progress.
IOReturn InitHeadphone::setAwakeAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3)
{
    (void)arg1; (void)arg2; (void)arg3;
    InitHeadphone *self = OSDynamicCast(InitHeadphone, owner);
    if (self == nullptr || self->timer == nullptr)
        return kIOReturnSuccess;

    self->attempt = 0;
    if (arg0 != nullptr) {
        self->awake = true;
        self->trigger = "wake";
        self->timer->setTimeoutMS(kWakeDelayMS);
    } else {
        self->awake = false;
        self->timer->cancelTimeout();
    }
    return kIOReturnSuccess;
}

// Root domain messages. Returning kIOReturnSuccess acknowledges at once, so sleep is never delayed
// except by an init that is already running (a few milliseconds).
IOReturn InitHeadphone::powerMessage(void *target, void *refCon, UInt32 messageType,
                                     IOService *provider, void *messageArgument, vm_size_t argSize)
{
    (void)refCon; (void)provider; (void)messageArgument; (void)argSize;
    InitHeadphone *self = static_cast<InitHeadphone *>(target);
    if (self == nullptr || self->workLoop == nullptr)
        return kIOReturnSuccess;

    switch (messageType) {
    case kIOMessageSystemWillSleep:
        self->workLoop->runAction(&InitHeadphone::setAwakeAction, self, nullptr);
        break;
    case kIOMessageSystemHasPoweredOn:
        self->workLoop->runAction(&InitHeadphone::setAwakeAction, self, reinterpret_cast<void *>(static_cast<uintptr_t>(1)));
        break;
    default:
        break;
    }
    return kIOReturnSuccess;
}
