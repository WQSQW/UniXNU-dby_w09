/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* DbyTorch: the DBY-W09 (Huawei MatePad 11, SM8250/kona) LED torch from XNU - PM8150L's flash-v2
 * block at PPID 0x5d3, channel 0, reached through the SM8250 v5 SPMI arbiter as apps EE 0. See
 * README.md. Structure follows PaganiTorch's PaganiTorch.h; every number follows
 * leds-qpnp-flash-v2.c and this unit's stock DTBO, recorded in DbyTorchRegs.h.
 *
 * Boot arguments (all optional):
 *   dby-torch=N           0 or absent: off (nothing runs, maps, allocates or registers); 1 on;
 *                         2 dry run: discovery, identity, owner and a register dump in the boot log
 *                         (observer reads only) - nothing is written
 *   dby-torch-max-ma=MA   the current at level 1000 (default 300 = [D] qcom,current-ma; never above
 *                         the module's own [D] qcom,max-current = 500, which is what the write gate
 *                         will ever sign)
 *   dby-torch-test=MS     bring-up: on at level 1000 for MS ms (at most 1000) right after the
 *                         initialization, then off (mode 1 only)
 *   dby-torch-log=N       0 failures only, 1 rationed (default), 2 every change
 *   dby-torch-ios=N       0 or absent: no iOS flashlight surface (DbyTorchIos.h and
 *                         DbyTorchIos.cpp never run); 1: publish the AppleH6CamIn service, its
 *                         mediaserverd-only user client and IODeviceTree:/product/camera "flash",
 *                         just before launchd - and only when this driver runs at mode 1 and the
 *                         H6ISP.mediacapture plug-in is visible on the root, so the Control Center
 *                         button is never a button that does nothing
 *
 * An "on" ends by itself after 5 minutes; any bus failure, refusal, failed read-back or fault bit
 * switches the LED off, closes the write gate and keeps the driver off until reboot. */
#ifndef DBY_TORCH_H
#define DBY_TORCH_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Stage7Probe's call, after the display and the input have come up. Returns at once: it reads the
 * arguments and starts one kernel thread; nothing about the boot waits on the bus. */
void dby_torch_start(void);

/* Level 0: off; 1..1000: on at that permille of dby-torch-max-ma (a level above 0 is at least one
 * resolution step, 12 mA). Any thread that may sleep, never from interrupt context: it waits for the
 * driver's thread, at most 200 ms.
 * 0 done, -19 the driver is not running (off, dry run, refused or after a fault), -22 bad level,
 * -5 this request hit a fault or a bus error (the LED is off), -1 the write gate would not sign it,
 * -110 the thread did not answer in time. The 5 minute limit runs from the moment the LED went on:
 * changing the level does not extend it. */
int dby_torch_set(uint32_t level);
/* The same for a named caller, for the log. */
int dby_torch_request(uint32_t level, const char *who);
/* The level now (0 off), or -19 when the driver is not running. */
int dby_torch_level(void);

/* Diagnostics - the words of dbt_core_export (DbyTorchCore.c), one int per line of the boot log's
 * story: [0] state (0 off, 1 on, 2 dry run, 3 refused, 4 fault), [1] stage (1 config, 2 arbiter,
 * 3 write path, 4 known off, 5 identity dumped; negative: where it stopped, -1 config, -2 arbiter,
 * -3 register dump, -4 write path open, and the three the glue answers itself: -5 no windows,
 * -6 no thread, -7 no memory), [2] level, [3] mA,
 * [4] TGR_CURRENT code, [5] IRES index, [6] faults, [7] ons, [8] offs, [9] derates, [10] SPMI reads,
 * [11] SPMI writes, [12] gate refusals, [13] bus errors, [14] the flash channel's APID,
 * [15] the last arbiter STATUS word. Negative [1] values are written here before the thread ends. */
extern volatile int32_t dby_torch_status[16];

#ifdef __cplusplus
}
#endif
#endif
