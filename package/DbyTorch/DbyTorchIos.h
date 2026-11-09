/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The flashlight of iOS without a camera (boot argument dby-torch-ios=1): what DbyTorchIos.cpp
 * makes, decided here in portable C so the host tests run the same rules. PaganiTorch's TorchIos.h
 * is the shape; the evidence below is this board's own.
 *
 * The chain (iPadOS 17.7.11 / 21H461, the root this board boots, as PaganiTorch read it and as
 * DbyTorchIos.cpp's gate re-measures it): Control Center's FlashlightModule and the lock screen ask
 * +[AVFlashlight hasFlashlight], which is MobileGestalt's camera-flash: the property "flash" of
 * IODeviceTree:/product/camera exists (any value). AVFlashlight talks over XPC to FigFlashlight in
 * mediaserverd. FigFlashlight takes a device and a stream from BWFigCaptureDeviceVendor and sets the
 * stream's TorchLevel. The vendor, once in the process, picks its capture service with
 * FigCaptureGetCaptureDeviceCreateFunction: for each of AppleH16CamIn, H13, H10, H9, H6, when an
 * IOService of that class exists it dlopens /System/Library/MediaCapture/<H..>ISP.mediacapture.
 * Measured on this board's pinned kernelcache (out/ipsw/21H461/kernelcache.decompressed, 2026-11-09)
 * its classes are AppleH9CamIn, AppleH10CamIn and AppleH13CamIn: H6 is the last entry of the
 * vendor's table and is not one of the kernel's classes, so publishing it leaves the real camera
 * classes alone.
 *  - the kernel publishes an IOService of class AppleH6CamIn and its user client;
 *  - the plug-in torch/DbyTorchPlugin.m, /System/Library/MediaCapture/H6ISP.mediacapture through the
 *    Data seed and the boot addon mount, is loaded by that lookup. It does not export
 *    H6ISPCaptureDeviceCreate, so the vendor stays as it is today, without a capture device: no
 *    camera appears anywhere. It answers the vendor's few messages for the client named
 *    FigFlashlight only, with a stream whose TorchLevel goes to the user client;
 *  - the kernel adds /product/camera with "flash", only when the plug-in is visible and the driver
 *    runs: never a button that does nothing.
 * Without dby-torch-ios nothing here runs and nothing is logged. */
#ifndef DBY_TORCH_IOS_H
#define DBY_TORCH_IOS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t create;            /* 1: publish the service and the node */
    const char *why;           /* why not: "off", or the refusal for the boot log */
} DbtIosPlan;

/* The plan for the boot arguments dby-torch-ios (ios) and dby-torch (torch). 0: create, or off
 * (ios 0); -1: refused (plan->why). */
int dbt_ios_plan(unsigned ios, unsigned torch, DbtIosPlan *plan);
/* The plan just before launchd: driver_ready, the torch driver answers; plugin_visible, the addon
 * mount reached its fifth stage. 0, with create as *plan had it; -1: refused (final->why). */
int dbt_ios_final(const DbtIosPlan *plan, int driver_ready, int plugin_visible, DbtIosPlan *final);
/* The level of an IOConnect call's scalar: 0..1000, else -22. */
int dbt_ios_level(uint64_t scalar);

/* The kernel half (DbyTorchIos.cpp). The plan is made in dby_torch_start; the commit is called by
 * the exec patch (patches/xnu/zze-dby-torch-media-mount.patch) after the MediaCapture mount, on the
 * thread that goes on to exec launchd. */
void dby_torch_ios_plan(unsigned ios, unsigned torch);
void dby_torch_ios_commit(int plugin_visible);
/* [0] 1 planned, 2 made; negative: -1 plan refused, -2 no /product, -3 no memory or a property
 * failed, -4 service, -5 final refusal. [1] user client opens, [2] level calls, [3] failed level
 * calls. */
extern volatile int32_t dby_torch_ios_status[4];

/* Names. */
#define DBT_IOS_PARENT       "IODeviceTree:/product"
#define DBT_IOS_NODE         "camera"
#define DBT_IOS_FLASH        "flash"
#define DBT_IOS_SERVICE      "AppleH6CamIn"            /* CMCapture's table, the last entry */
#define DBT_IOS_CLIENT       "DbyTorchUserClient"
#define DBT_IOS_PROCESS      "mediaserverd"            /* the only process that may open it */
#define DBT_IOS_SELECTOR_SET 0u                        /* one scalar in: the level, 0..1000 */

#ifdef __cplusplus
}
#endif
#endif
