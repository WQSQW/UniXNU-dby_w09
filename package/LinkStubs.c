// SPDX-License-Identifier: BSD-2-Clause-Patent
// No-op definitions of the probe hooks the patched XNU references while alioth_defconfig leaves their component out.
// Each hook runs only on its boot argument, which board.toml does not pass; the kernel link needs the symbol anyway.

// offline-component-load-probes.patch, bsd_init: called on offline-legacy-register=1.
// CONFIG_XNU_NATIVE_LEGACY_DISPLAY is now on, so libNativeLegacyDisplay.late.a defines this
// entry and the kernel calls the real one. The stub is gone: keeping it was a duplicate symbol
// at the link. Set the displayed switch back to n and this definition has to come back with it.
#include <stdint.h>

#if !XNU_NATIVE_LEGACY_DISPLAY
// Weak here, unlike the alioth original: on this Linux build scripts/inject-linux-link-stubs.py
// (build-xnu-4k-baseline.sh:82) already defines this hook inside bsd_init.o, and a second strong
// definition is a hard `duplicate symbol` at the link. alioth escapes that because it enables
// CONFIG_XNU_NATIVE_LEGACY_DISPLAY, which takes the #if !XNU_NATIVE_LEGACY_DISPLAY branch out.
// Weak keeps the same decline-yourself behaviour while letting the kernel-side stub win.
__attribute__((weak)) void native_legacy_display_probe(void);
__attribute__((weak)) void native_legacy_display_probe(void)
{
}

#endif

// ShengUfs imports this as a weak reference, which a kernel link still has to resolve.
// alioth leaves CONFIG_XNU_BOOT_PROGRESS unset, so libBootProgress.a is not linked.
// Weak here, so enabling that option later keeps the real definition.
__attribute__((weak)) void boot_progress_poll(void);
__attribute__((weak)) void boot_progress_poll(void)
{
}

// offline-owner-systemos-mount.patch, kern_exec: native_owner_data_prepare() calls
// native_gpu_addon_prepare(), which tests native_ram_block_gpu_addon before calling it. The patch
// declares that symbol weak_import, and ld64 still refuses to leave a weak_import undefined in a
// static kernel link, so CONFIG_XNU_OWNER_DATA_MOUNT does not link until this name resolves.
// libRamBlock.a owns the real definition, but alioth leaves CONFIG_XNU_NATIVE_RAM_BLOCK unset: the
// Data mount reads the DarwinBoot RAMDisk through mdevlookup(0), not a virtio provider.
// The hook runs only on sheng-gpu-addon=1, which board.toml does not pass. Weak here, so
// selecting the RamBlock transport later keeps the real definition.
__attribute__((weak)) int native_ram_block_gpu_addon(const unsigned char **, unsigned long long *);
__attribute__((weak)) int native_ram_block_gpu_addon(const unsigned char **addon, unsigned long long *bytes)
{
	(void)addon;
	(void)bytes;
	return 0;
}

// GopFramebuffer's compositor-scanout path imports the UFS DPU window as weak_import, and a
// static kernel link refuses to leave a weak_import undefined. NativeUfs defines both; this board
// selects ShengUfs, so neither exists here. NativeLegacyDisplay.cpp:136 tests `&native_ufs_dpu_map`
// and `!(native_legacy_dpu_state >> 31)` before using either, so a zero state and a reporter that
// maps nothing make the path decline itself, which is the correct answer on a board whose DPU is
// not published. Weak, so a component that later defines the real symbols wins.
__attribute__((weak)) volatile uint32_t native_ufs_dpu_state;
__attribute__((weak)) uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages,
    unsigned count);
__attribute__((weak)) uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages,
    unsigned count)
{
	(void)slot;
	(void)pages;
	(void)count;
	return 0;
}

// BootProgress.c:1070 calls boot_log_tick() as a weak_import, and the official provider of that
// counter is package/BootLog.c (sheng 38,585 B / alioth 81,678 B), which our device repo does not ship.
// Same rule as the boot_progress_poll stub above: a static kernel link refuses an undefined weak_import,
// so the hook gets a no-op here; weak, so linking the real BootLog.c later wins without editing this file.
__attribute__((weak)) void boot_log_tick(unsigned now); 
__attribute__((weak)) void boot_log_tick(unsigned now) { (void)now; }

// libNativeLegacyDisplay.late.a: native_legacy_display_late_probe() starts by running the
// component probes that share its boot-thread slot. alioth ships them for real
// (package/KeyStoreProbe.cpp, HidInputProbe.cpp, TouchProbe.cpp, CpuFreqProbe.cpp,
// GpuPowerProbe.cpp, WifiProbe.cpp, AdspProbe.cpp, BatteryProbe.cpp, AudioProbe.cpp,
// ButtonsProbe.cpp, BacklightProbe.cpp); this board selects none of those components
// yet, so each hook is a weak no-op a later real definition wins over. Weak matters:
// the display archive is force-loaded into the same link.

__attribute__((weak)) void native_hid_input_probe(void);
__attribute__((weak)) void native_hid_input_probe(void)
{
}

__attribute__((weak)) void native_keystore_probe(void);
__attribute__((weak)) void native_keystore_probe(void)
{
}

__attribute__((weak)) void native_touch_probe(void);
__attribute__((weak)) void native_touch_probe(void)
{
}

__attribute__((weak)) void sheng_cpufreq_probe(void);
__attribute__((weak)) void sheng_cpufreq_probe(void)
{
}

__attribute__((weak)) void sheng_gpu_power_probe(void);
__attribute__((weak)) void sheng_gpu_power_probe(void)
{
}

__attribute__((weak)) void sheng_wifi_probe(void);
__attribute__((weak)) void sheng_wifi_probe(void)
{
}

__attribute__((weak)) void sheng_adsp_probe(void);
__attribute__((weak)) void sheng_adsp_probe(void)
{
}

__attribute__((weak)) void sheng_battery_probe(void);
__attribute__((weak)) void sheng_battery_probe(void)
{
}

__attribute__((weak)) void sheng_audio_probe(void);
__attribute__((weak)) void sheng_audio_probe(void)
{
}

__attribute__((weak)) void sheng_buttons_probe(void);
__attribute__((weak)) void sheng_buttons_probe(void)
{
}

__attribute__((weak)) void sheng_backlight_probe(void);
__attribute__((weak)) void sheng_backlight_probe(void)
{
}

__attribute__((weak)) void sheng_dpu_probe(void);
__attribute__((weak)) void sheng_dpu_probe(void)
{
}
