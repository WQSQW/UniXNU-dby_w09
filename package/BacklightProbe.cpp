// SPDX-License-Identifier: BSD-2-Clause-Patent
// Starts the panel brightness service (AppleARMBacklight.cpp) with boot argument dby-bl=1: registers that file's classes with the
// kernel's module (its static initializer, which the build takes out of __mod_init_func), then lets it set up the engine and publish.
// The file name of the service is kept from the sheng board so the initialiser symbol below is the one its build produces; the class
// inside is this board's (DbyBacklight), and the hardware constants are dby_w09's LP8556 rather than sheng's KTZ8866.
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_backlight_static_init(void) __asm__("__GLOBAL__sub_I_AppleARMBacklight.cpp");
extern "C" void dby_backlight_start(void);
// See AppleARMBacklight.cpp for the meaning of each entry.
extern "C" {volatile int32_t native_backlight_stats[16];}

// 四颗 smartpa（DbySmartPA.cpp）。挂在这条序列里，是因为点屏是所有设备初始化的既定位置；
// 它自己看 dby-pa 参数，不在就什么都不做。
extern "C" void dby_smartpa_probe(void) __attribute__((weak_import));

extern "C" void sheng_backlight_probe(void) {
    if (dby_smartpa_probe) dby_smartpa_probe();
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("dby-bl", &enabled, sizeof enabled) || enabled != 1 || native_backlight_stats[0]) return;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {native_backlight_stats[9] = -12; native_backlight_stats[0] = -5; return;}
    native_backlight_static_init();
    const OSReturn status = OSMetaClass::postModLoad(handle);
    if (status) {native_backlight_stats[9] = status; native_backlight_stats[0] = -5; return;}
    dby_backlight_start();
}
