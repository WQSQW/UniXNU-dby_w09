// SPDX-License-Identifier: BSD-2-Clause-Patent
// Panel brightness for the dby_w09 board (boot argument dby-bl=1, started by BacklightProbe.cpp).
//
// Derived from UniXNU-sheng's package/AppleARMBacklight.cpp (KTZ8866, two engines, 11-bit). The structure, the event-driven transfer
// handling, the fade stepping and the impersonation of AppleARMBacklight are kept exactly as they are there; only the hardware
// constants below were replaced with this board's, read from the board's own kernel device tree and clock driver:
//
//   panel backlight   one LP8556 at I2C address 0x2c on qupv3_se11 (i2c@a8c000), GIC SPI 356
//                     kernel: arch/arm64/boot/dts/vendor/qcom/kona/DBY/W09_VA/huawei-lcd.dtsi "hw_backlight@2c",
//                     compatible "huawei,hw_lp855x", bl_type_string "8556"
//   level registers   0x10 = eight least significant bits, 0x11 low nibble = the four most significant: 12 bits, 1..4095
//                     kernel: backlight_ic_level_lsb_cmd = <0x08 0x10 0x00 0xff>, backlight_ic_level_msb_cmd = <0x04 0x11 0x00 0x0f>
//                     (the leading 8 and 4 are the bit counts of each part), backlight_ic_backlight_level = 3932
//   engine clocks     gcc-kona.c: wrap1 s3 (0x1839c, vote bit 25), m_ahb (0x18004, bit 20), s_ahb (0x18008, bit 21),
//                     core (0x23138, bit 19), core_2x (0x23140, bit 18), all voted in 0x52008 -- the same QUP wrapper vote
//                     register offset the sheng board uses
//
// One engine and one device instead of two of each, so the engine array holds a single entry (kEngines).
//
// The engines run the GENI I2C firmware the boot loaders left loaded and clocked; this code votes their clocks on again, keeps the clock
// and SCL timing the boot loader set and only issues FIFO transfers through the event-driven core in GeniI2c.c. It does not program the
// controller from scratch: geni_i2c_prepare() only checks that the firmware, the clock and the SCL counters are already in place and
// fails (rather than reconfiguring) if the boot loader did not leave the engine usable.
//
// Transfers never wait: the engine's interrupt (SPI 356, level) only reads and clears the engine's status, records it and signals the
// service's work loop, which advances the transfer. Until the interrupt is attached (the GIC driver may start later; attaching is
// retried every 250 ms from a timer) the work loop polls the status every millisecond while a transfer runs. A step that gets no answer
// within 25 ms is cancelled, then aborted. A newer level replaces one not yet written, so a fade never queues up.
//
// The class cannot be called AppleARMBacklight: AppleARMPlatform already defines that class, and a duplicate name panics the kernel when
// this file's classes are registered (int-229); the service answers user space's conformance query for it instead (publishFirst).
// CoreBrightness (backboardd) looks for a backlight service with backlight-control = true, reads min and max from IODisplayParameters
// brightness and sets {brightness, FadePeriod (16.16 seconds)} through setProperties. A fade is stepped every 16 ms by a timer on the
// service's own work loop.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <pexpert/pexpert.h>
#include "GeniI2c.h"

// [0] stage (1 mapped, 2 engine ready, 3 published, 4 published as AppleARMBacklight, negative: the step that failed), [1] brightness
// requests, [2] last level written, [3] level read back from the engine at start, [4] unused (there is one engine on this board, kept at
// -1 so the layout stays the one the log reader expects), [5] transfer errors, [6] last M_IRQ status of a failed transfer, [7] engine
// protocol and clock configuration (proto << 24 | M_CLK_CFG), [8] engine IF_DISABLE bit 0 and GENI_SCL_COUNTERS when prepare failed,
// [9] last error (the GeniI2c.h error, or a setup code), [10] interrupts from the engine, [11] unused, [12] interrupt mode (0 polling,
// 1 attached, else the attach result), [13] transfer timeouts, [14] attach tries, [15] transfers done
extern "C" {extern volatile int32_t native_backlight_stats[16];}
// The last brightness asked for: [0] level, [1] fade in milliseconds; [2] request timer runs, [3] fades begun from a request; [4] interrupts
// with no status bit set, [5] the last status an interrupt saw.
extern "C" {volatile int32_t native_backlight_request[6];}
extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));

namespace {
constexpr uint64_t kGcc = 0x100000, kSe = 0xa8c000;
constexpr uint32_t kSpi = 356, kEngines = 1;
constexpr uint32_t kGccVote = 0x52008;
// gcc-kona.c: wrap1 core_2x 18, core 19, m_ahb 20, s_ahb 21, s3 25. Only wrap1 is voted: the backlight engine is the only QUP this file
// touches, and the touch driver votes its own engine.
constexpr uint32_t kVoteBits = (1u << 18) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 25);
// The matching halt registers, in gcc-kona.c order: wrap1 core_2x, core, m_ahb, s_ahb, s3.
constexpr uint32_t kHalt[] = {0x23140, 0x23138, 0x18004, 0x18008, 0x1839c};
constexpr uint8_t kAddress = 0x2c, kBrtLsb = 0x10, kBrtMsb = 0x11;
constexpr uint32_t kMaxLevel = 4095, kStepMs = 16, kPollMs = 1, kStepDeadlineMs = 25, kAttachMs = 250, kAttachTries = 240;
// The panel, not the register, sets the usable range: lcdkit3.0/debussy/debussy_dual_nt36523_video_mode_default.dtsi has
// lcd-kit,panel-bl-max = <3932> and lcd-kit,panel-bl-min = <75>, and lcd_kit_mipi_set_backlight()
// (lcdkit3.0/bootable/common/src/lcd_kit_common.c:721) clamps to them but leaves an exact 0 alone. Below the floor the
// LP8556 emits nothing the eye can see, which is why the bottom of the slider looked like a black screen.
constexpr uint32_t kPanelMax = 3932, kPanelMin = 75;

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
Window gGcc, gSe;
volatile uint32_t gIrqStatus;
IOInterruptEventSource *volatile gInbox;
// The newest brightness asked for: goal << 16 | fade milliseconds, with bit 63 set while the work loop has not taken it.
volatile uint64_t gRequest;

bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
uint32_t rd(volatile uint32_t *base, uint32_t reg) {return base[reg / 4];}
void wr(volatile uint32_t *base, uint32_t reg, uint32_t value) {base[reg / 4] = value; __asm__ volatile("dsb sy" ::: "memory");}
uint32_t ioRead(void *ctx, uint32_t reg) {return rd(static_cast<volatile uint32_t *>(ctx), reg);}
void ioWrite(void *ctx, uint32_t reg, uint32_t value) {wr(static_cast<volatile uint32_t *>(ctx), reg, value);}
void fail(int step, int error) {native_backlight_stats[9] = error; native_backlight_stats[0] = -step;}
void bump(int index) {native_backlight_stats[index] = native_backlight_stats[index] + 1;}

// Interrupt context: read and clear the engine's status, keep it for the work loop and signal it. Nothing else.
void seIrq(void *) {
    volatile uint32_t *se = gSe.va;
    if (!se) return;
    const uint32_t status = rd(se, GENI_M_IRQ_STATUS);
    // The TX watermark is a level: it is raised again as soon as it is cleared while the FIFO is below the watermark, which stays so
    // until the work loop puts the data in. Cleared here alone it kept the line up (int-279: 25 million interrupts per engine in 300 s,
    // the work loop starved and ran 2 of 188 brightness requests), so the watermark is turned off here; the status bit still tells the
    // work loop to fill the FIFO.
    if (status & GENI_M_TX_WATERMARK) wr(se, GENI_TX_WATERMARK, 0);
    if (status) wr(se, GENI_M_IRQ_CLEAR, status);
    else native_backlight_request[4] = native_backlight_request[4] + 1;
    native_backlight_request[5] = (int32_t)status;
    __atomic_fetch_or(&gIrqStatus, status, __ATOMIC_RELEASE);
    bump(10);
    IOInterruptEventSource *inbox = gInbox;
    if (inbox && status) inbox->interruptOccurred(nullptr, nullptr, 0);
}
}  // namespace

class DbyBacklight : public IOService {
    OSDeclareDefaultStructors(DbyBacklight)
    // What the engine is doing: reading the level at start, then writing LSB and MSB of the newest level asked for.
    enum Step : uint8_t {StepNone, StepReadLsb, StepReadMsb, StepWriteLsb, StepWriteMsb};
    struct Engine {GeniI2c bus; Step step; bool started, usable; uint8_t lsb; int32_t written; uint32_t sending; uint64_t deadline;};
    Engine engine[kEngines] = {};
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *fade = nullptr, *tick = nullptr, *attachTimer = nullptr, *request = nullptr;
    IOInterruptEventSource *inbox = nullptr;
    uint32_t level = 0, from = 0, to = 0, target = 0, attachTries = 0;
    uint64_t start = 0, period = 0;
    bool irqMode = false, published = false;

    static uint64_t now() {return mach_absolute_time();}
    static uint64_t after(uint32_t ms) {uint64_t delta = 0; nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &delta); return now() + delta;}

    // Starts the engine's next transfer when it is free.
    void kick(unsigned i) {
        Engine &e = engine[i];
        if (!e.usable || geni_i2c_busy(&e.bus)) return;
        int rc = 0;
        if (!e.started) {e.step = StepReadLsb; rc = geni_i2c_start_read(&e.bus, kBrtLsb);}
        else if (e.written != (int32_t)target) {e.sending = target; e.step = StepWriteLsb; rc = geni_i2c_start_write(&e.bus, kBrtLsb, (uint8_t)(target & 0xff));}
        else {e.step = StepNone; return;}
        if (rc) {e.step = StepNone; return;}
        e.deadline = after(kStepDeadlineMs);
    }
    void noteFailure(unsigned i) {
        const Engine &e = engine[i];
        bump(5);
        native_backlight_stats[6] = (int32_t)e.bus.error_status;
        native_backlight_stats[9] = (int32_t)(i << 8 | e.bus.error);
        native_backlight_stats[13] = (int32_t)e.bus.timeouts;
    }
    // A transfer ended: the next step of the engine's sequence.
    void finished(unsigned i, int result) {
        Engine &e = engine[i];
        const bool ok = result == GeniDone;
        if (!ok) noteFailure(i);
        else bump(15);
        int rc = 0;
        switch (e.step) {
        case StepReadLsb:
            if (ok) {e.lsb = e.bus.data; rc = geni_i2c_start_read(&e.bus, kBrtMsb); e.step = StepReadMsb; break;}
            e.started = true; e.written = -1; native_backlight_stats[3 + i] = -1;
            break;
        case StepReadMsb:
            e.started = true;
            // 0x10 is the whole low byte, 0x11 low nibble the high four bits.
            e.written = ok ? (int32_t)(((uint32_t)e.bus.data << 8) | (e.lsb & 0xff)) : -1;
            native_backlight_stats[3 + i] = e.written;
            break;
        case StepWriteLsb:
            if (ok) {rc = geni_i2c_start_write(&e.bus, kBrtMsb, (uint8_t)((e.sending >> 8) & 0x0f)); e.step = StepWriteMsb; break;}
            e.written = (int32_t)e.sending;  // given up: written again with the next level asked for
            break;
        case StepWriteMsb:
            e.written = (int32_t)e.sending;
            if (ok) native_backlight_stats[2] = (int32_t)e.sending;
            break;
        default:
            break;
        }
        if (rc) noteFailure(i);
        if (geni_i2c_busy(&e.bus)) {e.deadline = after(kStepDeadlineMs); return;}
        e.step = StepNone;
        if (!published && engine[0].started) publishFirst();
        kick(i);
    }
    void advance(unsigned i, uint32_t status) {
        Engine &e = engine[i];
        if (!geni_i2c_busy(&e.bus)) return;
        const int result = geni_i2c_event(&e.bus, status);
        if (result != GeniPending) finished(i, result);
    }
    void armTick() {
        bool busy = false;
        uint64_t soonest = UINT64_MAX;
        for (const Engine &e : engine) if (geni_i2c_busy(&e.bus)) {busy = true; if (e.deadline < soonest) soonest = e.deadline;}
        if (!busy) {tick->cancelTimeout(); return;}
        if (!irqMode) {tick->setTimeoutMS(kPollMs); return;}
        tick->wakeAtTime((AbsoluteTime)soonest);
    }
    // The work loop's view of the engine: what the interrupt gathered, in polling mode the status register itself, then the deadline.
    void service() {
        for (unsigned i = 0; i < kEngines; ++i) {
            uint32_t status = __atomic_exchange_n(&gIrqStatus, 0u, __ATOMIC_ACQUIRE);
            if (!irqMode) {
                const uint32_t direct = rd(gSe.va, GENI_M_IRQ_STATUS);
                if (direct) wr(gSe.va, GENI_M_IRQ_CLEAR, direct);
                status |= direct;
            }
            if (status) advance(i, status);
            Engine &e = engine[i];
            if (geni_i2c_busy(&e.bus) && now() >= e.deadline) {
                const int result = geni_i2c_timeout(&e.bus);
                native_backlight_stats[13] = (int32_t)e.bus.timeouts;
                if (result != GeniPending) finished(i, result);
                else e.deadline = after(kStepDeadlineMs);
            }
        }
        armTick();
    }
    static void inboxFired(OSObject *owner, IOInterruptEventSource *, int) {
        DbyBacklight *self = static_cast<DbyBacklight *>(owner);
        if (!self) return;
        self->service();
    }
    // A brightness request on the work loop (setProperties arms this timer at once): the newest one asked for, if not yet taken.
    static void requestFired(OSObject *owner, IOTimerEventSource *) {
        DbyBacklight *self = static_cast<DbyBacklight *>(owner);
        if (!self) return;
        native_backlight_request[2] = native_backlight_request[2] + 1;
        const uint64_t request = __atomic_exchange_n(&gRequest, 0ull, __ATOMIC_ACQUIRE);
        if (request >> 63) {native_backlight_request[3] = native_backlight_request[3] + 1; self->beginFade((uint32_t)(request >> 16) & 0xffff, (uint32_t)request & 0xffff);}
    }
    static void tickFired(OSObject *owner, IOTimerEventSource *) {
        if (DbyBacklight *self = static_cast<DbyBacklight *>(owner)) self->service();
    }
    // Attaching the line, retried while the GIC driver is not up yet; the engine keeps working by polling meanwhile.
    static void attachFired(OSObject *owner, IOTimerEventSource *sender) {
        DbyBacklight *self = static_cast<DbyBacklight *>(owner);   // see impersonate()
        if (!self || self->irqMode) return;
        if (!armcpu_spi_attach) {native_backlight_stats[12] = -1; return;}
        native_backlight_stats[14] = (int32_t)++self->attachTries;
        // armcpu_spi_attach takes a GIC INTID, which is the SPI number plus 32.
        int rc = armcpu_spi_attach(32 + kSpi, false, seIrq, nullptr, nullptr, nullptr);
        if (rc == kIOReturnNotReady && self->attachTries < kAttachTries) {sender->setTimeoutMS(kAttachMs); return;}
        if (rc) {native_backlight_stats[12] = rc; return;}
        self->irqMode = true;
        native_backlight_stats[12] = 1;
        self->service();
    }

    void setLevel(uint32_t value) {
        target = value;
        kick(0);
        armTick();
    }
    static void fadeStep(OSObject *owner, IOTimerEventSource *sender) {
        DbyBacklight *self = static_cast<DbyBacklight *>(owner);   // see impersonate()
        if (!self) return;
        const uint64_t elapsed = now() - self->start;
        uint32_t next = self->to;
        if (elapsed < self->period)
            next = (uint32_t)((int64_t)self->from + ((int64_t)self->to - (int64_t)self->from) * (int64_t)elapsed / (int64_t)self->period);
        if (next != self->level) {self->setLevel(next); self->level = next;}
        if (next != self->to) sender->setTimeoutMS(kStepMs);
    }
    // On the work loop: a fade from the level now shown to goal.
    void beginFade(uint32_t goal, uint32_t milliseconds) {
        fade->cancelTimeout();
        from = level;
        to = goal;
        start = now();
        nanoseconds_to_absolutetime((uint64_t)milliseconds * NSEC_PER_MSEC, &period);
        fadeStep(this, fade);
    }
    static IOReturn begin(OSObject *owner, void *, void *, void *, void *) {
        DbyBacklight *self = static_cast<DbyBacklight *>(owner);   // see impersonate()
        if (!self) return kIOReturnBadArgument;
        self->kick(0);
        if (!self->published && self->engine[0].started) self->publishFirst();
        self->armTick();
        self->attachTimer->setTimeoutMS(1);
        return kIOReturnSuccess;
    }
    void publish(uint32_t value) {
        OSDictionary *brightness = OSDictionary::withCapacity(3), *parameters = OSDictionary::withCapacity(1);
        OSNumber *min = OSNumber::withNumber(0ULL, 32), *max = OSNumber::withNumber(kMaxLevel, 32), *current = OSNumber::withNumber(value, 32);
        if (brightness && parameters && min && max && current) {
            brightness->setObject("min", min); brightness->setObject("max", max); brightness->setObject("value", current);
            parameters->setObject("brightness", brightness);
            setProperty("IODisplayParameters", parameters);
        }
        OSSafeReleaseNULL(min); OSSafeReleaseNULL(max); OSSafeReleaseNULL(current); OSSafeReleaseNULL(brightness); OSSafeReleaseNULL(parameters);
    }
    // The service presents itself as AppleARMBacklight (AppleARMPlatform's class, loaded but without a device on this board): its
    // vtable is copied and getMetaClass (slot 7) answers that metaclass. IOObjectConformsTo and the sandbox both go through it: the
    // sandbox decides iokit-set-properties by walking getMetaClass's superclass chain by name, and backboardd's profile lets it set
    // the brightness keys on AppleARMBacklight only (int-275: "deny iokit-set-properties iokit-class:ShengBacklight property:FadePeriod").
    // AppleARMBacklight and IOService stay in that chain, so OSDynamicCast to IOService works; to DbyBacklight it does not, which is
    // why the event source actions cast statically. No loaded kext looks this class up (AppleSPU, AppleEmbeddedLightSensor and
    // AppleM68Buttons do, and are not part of this kernel), and the service is never freed.
    static inline const OSMetaClass *gAppleClass = nullptr;
    static inline bool gImpersonating = false;
    static const OSMetaClass *appleMetaClass(const OSMetaClassBase *) {return gAppleClass;}
    bool impersonate() {
        enum {kSlots = 320, kGetMetaClass = 7};
        const OSSymbol *name = OSSymbol::withCString("AppleARMBacklight");
        gAppleClass = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
        OSSafeReleaseNULL(name);
        if (!gAppleClass) return false;
#if __has_feature(ptrauth_calls)
        // arm64e: the vtable pointer and its entries are signed, so a copied vtable would not authenticate. getMetaClass itself
        // answers the AppleARMBacklight metaclass once this flag is set (its definition below the class).
        __atomic_store_n(&gImpersonating, true, __ATOMIC_RELEASE);
        return true;
#endif
        uintptr_t *copy = static_cast<uintptr_t *>(IOMalloc(kSlots * sizeof(uintptr_t)));
        if (!copy) return false;
        memcpy(copy, *reinterpret_cast<uintptr_t **>(this), kSlots * sizeof(uintptr_t));
        copy[kGetMetaClass] = reinterpret_cast<uintptr_t>(&appleMetaClass);
        __asm__ volatile("dmb ish" ::: "memory");
        *reinterpret_cast<uintptr_t **>(this) = copy;
        return true;
    }
    // The engine answered (or gave up) the level read: publish what the panel shows now and let CoreBrightness find the service.
    void publishFirst() {
        published = true;
        const int32_t read = native_backlight_stats[3];
        level = to = target = read > 0 ? (uint32_t)read : kMaxLevel / 2;
        // CoreBrightness finds the backlight by backlight-control = true and takes it for the built-in display's only when
        // IOObjectConformsTo(service, "AppleARMBacklight") holds (CBDisplayContaineriOS findBacklight). Only for the built-in display
        // does CoreBrightness start the libEDR server (com.apple.libEDR.Notification) that QuartzCore's IOMFBServer sends to
        // synchronously once backlight-control makes CADeviceHasBacklight true. int-236 published backlight-control without the
        // conformance: the server never started, the send blocked with the display server lock held and the screen stopped updating.
        // dby-bl-control=0 leaves both out.
        uint32_t control = 1;
        PE_parse_boot_argn("dby-bl-control", &control, sizeof control);
        // backboardd's sandbox profile refuses FadePeriod on this service (int-276), and CoreBrightness sends it with every brightness:
        // the grant (patches/xnu/zzwb-native-sandbox-grant-set-properties.patch) lets backboardd alone set its properties, which
        // setProperties checks itself.
        if (control == 1 && impersonate()) {setProperty("NativeSandboxGrantProcess", "backboardd"); setProperty("backlight-control", true);}
        const bool impersonated = control == 1 && gAppleClass;
        publish(level);
        registerService();
        native_backlight_stats[0] = impersonated ? 4 : 3;
    }
public:
    bool startBacklight(const bool usable[kEngines]) {
        loop = IOWorkLoop::workLoop();
        fade = IOTimerEventSource::timerEventSource(this, &DbyBacklight::fadeStep);
        tick = IOTimerEventSource::timerEventSource(this, &DbyBacklight::tickFired);
        attachTimer = IOTimerEventSource::timerEventSource(this, &DbyBacklight::attachFired);
        inbox = IOInterruptEventSource::interruptEventSource(this, &DbyBacklight::inboxFired);
        request = IOTimerEventSource::timerEventSource(this, &DbyBacklight::requestFired);
        if (!loop || !fade || !tick || !attachTimer || !inbox || !request) return false;
        if (loop->addEventSource(fade) || loop->addEventSource(tick) || loop->addEventSource(attachTimer) || loop->addEventSource(inbox) ||
            loop->addEventSource(request)) return false;
        for (unsigned i = 0; i < kEngines; ++i) {
            const GeniIo io = {ioRead, ioWrite, (void *)gSe.va};
            uint32_t config = 0;
            engine[i].usable = usable[i] && geni_i2c_prepare(&engine[i].bus, &io, kAddress, &config) == 0;
            native_backlight_stats[7 + i] = (int32_t)config;
            // Why prepare() refused, when it did: it checks the I2C firmware revision, the clock enable bit, the interface-disable
            // bit and the SCL counters, and reports all four through one -1. The first two are in config above; these are the other two.
            if (!engine[i].usable) native_backlight_stats[8] = (int32_t)((rd(gSe.va, GENI_IF_DISABLE) & 1u) | (rd(gSe.va, GENI_SCL_COUNTERS) ? 2u : 0u));
            engine[i].started = !engine[i].usable;
            engine[i].written = -1;
            if (!engine[i].usable) native_backlight_stats[3 + i] = -1;
        }
        if (!engine[0].usable) return false;
        gInbox = inbox;
        native_backlight_stats[0] = 2;
        loop->runAction(&DbyBacklight::begin, this);
        return true;
    }
    IOReturn setProperties(OSObject *properties) override {
        OSDictionary *dictionary = OSDynamicCast(OSDictionary, properties);
        if (!dictionary) return kIOReturnBadArgument;
        OSNumber *brightness = OSDynamicCast(OSNumber, dictionary->getObject("brightness"));
        if (!brightness) return kIOReturnSuccess;
        OSNumber *fadePeriod = OSDynamicCast(OSNumber, dictionary->getObject("FadePeriod"));
        const int32_t requested = (int32_t)brightness->unsigned32BitValue();
        uint32_t goal = requested < 0 ? 0 : (uint32_t)requested > kPanelMax ? kPanelMax : (uint32_t)requested;
        // The floor lives here and only here. Publishing it as the range's min (IODisplayParameters min=75) put CoreBrightness
        // and backboardd out of step on 2026-11-06: the screen stopped updating and the boot froze on the Apple logo. The range
        // the service advertises stays 0..4095, exactly what the build that booted advertised; 0 is left alone because the
        // display power path uses it for screen off.
        if (goal && goal < kPanelMin) goal = kPanelMin;
        uint64_t milliseconds = fadePeriod ? ((uint64_t)fadePeriod->unsigned32BitValue() * 1000) >> 16 : 0;
        if (milliseconds > 5000) milliseconds = 5000;
        bump(1);
        native_backlight_request[0] = (int32_t)goal;
        native_backlight_request[1] = (int32_t)milliseconds;
        // Handed to the work loop without waiting for it (a runAction here blocks backboardd's brightness thread on the work loop's
        // gate; int-277 got 53 requests in the first 41 s and none after): a newer request replaces one not yet taken. The request timer, not
        // the interrupt source, carries it: int-278 signalled the interrupt source for 345 requests and wrote none.
        __atomic_store_n(&gRequest, 1ull << 63 | (uint64_t)goal << 16 | milliseconds, __ATOMIC_RELEASE);
        request->setTimeoutUS(1);
        publish(goal);
        return kIOReturnSuccess;
    }
};
#if __has_feature(ptrauth_calls)
// OSDefineMetaClassAndStructors(DbyBacklight, IOService) with getMetaClass answering the AppleARMBacklight metaclass after impersonate().
DbyBacklight::MetaClass DbyBacklight::gMetaClass;
const OSMetaClass * const DbyBacklight::metaClass = &DbyBacklight::gMetaClass;
const OSMetaClass * const DbyBacklight::superClass = &IOService::gMetaClass;
DbyBacklight::DbyBacklight(const OSMetaClass *meta) : IOService(meta) {}
DbyBacklight::~DbyBacklight() {}
const OSMetaClass *DbyBacklight::getMetaClass() const
{
    return __atomic_load_n(&gImpersonating, __ATOMIC_ACQUIRE) && gAppleClass ? gAppleClass : &gMetaClass;
}
OSMetaClassConstructorInit(DbyBacklight, IOService, )
OSDefineDefaultStructors(DbyBacklight, IOService)
#else
OSDefineMetaClassAndStructors(DbyBacklight, IOService)
#endif

// Called by BacklightProbe.cpp once this file's classes are registered (its static initializer runs between preModLoad and postModLoad).
// The service starts with the engine's level read in flight and publishes itself when it is answered.
extern "C" void dby_backlight_start(void) {
    if (!mapWindow(gGcc, kGcc, 0x60000) || !mapWindow(gSe, kSe, 0x4000)) {fail(1, -12); return;}
    native_backlight_stats[0] = 1;
    wr(gGcc.va, kGccVote, rd(gGcc.va, kGccVote) | kVoteBits);
    for (uint32_t halt : kHalt) {
        // The AHB branches run under hardware clock gating (HW_CTL, bit 1 of the branch register): their CLK_OFF bit says off while the
        // bus is idle, so, as clk-branch.c does in that mode, they are not waited for (int-231 failed here on wrap 1 s_ahb, 0x23280).
        if (rd(gGcc.va, halt) & (1u << 1)) continue;
        unsigned us = 0;
        while ((rd(gGcc.va, halt) & (1u << 31)) && us < 200) {IODelay(1); ++us;}
        if (us == 200) {fail(2, (int)halt); return;}
    }
    const bool usable[kEngines] = {true};
    DbyBacklight *backlight = OSTypeAlloc(DbyBacklight);
    if (!backlight || !backlight->init() || !backlight->attach(IOService::getPlatform()) || !backlight->startBacklight(usable)) {
        if (backlight) backlight->release();
        fail(3, -19); return;
    }
    backlight->release();
}
