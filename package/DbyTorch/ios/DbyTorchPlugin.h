/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* What ios/DbyTorchPlugin.m shares with its host test. The names are DbyTorchIos.h's (scripts/build-dby-torch-plugin.py checks they match). */
#ifndef DBY_TORCH_PLUGIN_H
#define DBY_TORCH_PLUGIN_H
#include <stdint.h>

#define DBT_PLUGIN_SERVICE     "AppleH6CamIn"
#define DBT_PLUGIN_PROCESS     "mediaserverd"
#define DBT_PLUGIN_SELECTOR    0u
#ifndef DBT_PLUGIN_KEY_SYMBOL
#define DBT_PLUGIN_KEY_SYMBOL  "kFigCaptureStreamProperty_TorchLevel"    /* exported by CMCapture */
#endif
enum { DBT_PLUGIN_UNSUPPORTED = -12787, DBT_PLUGIN_FAILED = -12780 };   /* kCMBaseObjectError_ValueNotAvailable family: any nonzero fails the call */

/* FigFlashlight's level, 0.0 to 1.0, as the kernel's 0..1000: anything above 0 is at least 1. */
uint32_t dbt_plugin_level(float value);
/* Replaces the vendor's six methods, all or none. 0, or negative: nothing replaced. */
int dbt_plugin_install(void);
/* The kernel's user client (the host test provides its own). 0, or negative. */
int dbt_plugin_kernel_set(uint32_t level);
#endif
