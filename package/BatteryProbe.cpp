// SPDX-License-Identifier: BSD-2-Clause-Patent
// Starts the battery service (DbyBattery.cpp) on boot argument dby-battery=1: registers this board's
// classes with the kernel's module — its static initialiser, which the build takes out of
// __mod_init_func — and then lets the service map the arbiter and publish.
//
// This is upstream's package/BatteryProbe.cpp (alioth, the same SM8250) with three changes and nothing
// else: the boot argument is this board's dby- short name, the initialiser alias names this board's
// class file, and the start function is the one DbyBattery.cpp exports. The argument name costs 13
// bytes of the 1000-byte command-line cap (core/scripts/unixnu/payload.py:35) against the handover's
// sheng-battery at 15 — see board.toml's comment on the argument.
//
// The entry point keeps upstream's name, sheng_battery_probe, because the consumer already calls it:
// package/LegacyDisplayProbe.cpp:488 declares it and :500 calls it between sheng_adsp_probe() and
// sheng_audio_probe(). llvm-nm on that object spells the reference `_sheng_battery_probe U`, the
// unmangled C name, so this file's extern "C" definition is what resolves it — and it arrives through
// the force-loaded display archive, which is why it beats the weak no-op at package/LinkStubs.c:116-117
// (the built kernel shows _sheng_cpufreq_probe and _sheng_backlight_probe as strong `external` for
// exactly this mechanism, and _sheng_battery_probe as still `weak external` until this file joined).
//
// native_battery_stats[0] carries the stage, upstream's convention: positive is how far it got
// (1 class attached, 2 census run, 4 published — see DbyBattery.cpp), -10/-11 are the two module
// registration failures below, and DbyBattery.cpp records -1/-2/-3 for its own three steps.
//
// Every branch below prints. Until the 2026-11-06 round this file was the only silent one in the
// battery path, and that silence is what left "the console shows no battery rows" ambiguous: with
// no row here, a boot that never parsed dby-battery, a boot whose module registration failed and a
// boot whose PMIC never answered all looked identical from the host. kprintf, for the reason in
// DbyBattery.cpp's header: this board's COM4 mirrors kprintf and not IOLog.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_battery_static_init(void) __asm__("__GLOBAL__sub_I_DbyBattery.cpp");
extern "C" void dby_battery_start(void);
extern "C" {extern volatile int32_t native_battery_stats[16];}

extern "C" void sheng_battery_probe(void) {
    unsigned enabled = 0;

    // 2 and above is the write tier; DbyBattery reads the same argument and decides what it may
    // touch. The gate here only answers "does the service start at all".
    if (!PE_parse_boot_argn("dby-battery", &enabled, sizeof enabled) || enabled < 1) {
        kprintf("[battery] probe skipped: arg=%u (want 1 or 2)\n", enabled);
        return;
    }
    if (native_battery_stats[0]) {
        kprintf("[battery] probe skipped: stage already %d\n", native_battery_stats[0]);
        return;
    }
    kprintf("[battery] probe start\n");
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        native_battery_stats[0] = -10;
        kprintf("[battery] probe: preModLoad found no kmod for this image\n");
        return;
    }
    native_battery_static_init();
    const OSReturn status = OSMetaClass::postModLoad(handle);
    if (status) {
        native_battery_stats[0] = -11;
        kprintf("[battery] probe: postModLoad OSReturn=%u\n", (unsigned)status);
        return;
    }
    dby_battery_start();
    /* The service's own census and sample rows follow on this console; this row only says the
     * class made it onto the bus, which is the half nothing here said before. */
    kprintf("[battery] probe: classes registered, stage=%d\n", native_battery_stats[0]);
}
