/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 阶段 1a 的测试图（README.md）：不碰相机硬件，插件在 mediaserverd 里画一帧 NV12（420f）：滚动的彩条、帧号条、中间一个二维码。
 * 判据是代码扫描器出画面并识别出二维码，所以二维码按 ISO/IEC 18004 自己编，主机测试用 macOS 的 CoreImage 和 Vision 反解。
 * 这里是纯 C，只用 libc，不认识 CoreVideo：插件把锁好的两个平面传进来。 */
#ifndef DBY_CAMERA_PATTERN_H
#define DBY_CAMERA_PATTERN_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 版本 3、纠错 M、字节模式：29 模块，1 个 RS 块，44 数据码字加 26 纠错码字，最多 42 字节文本。 */
enum {
    DbcQrVersion = 3,
    DbcQrSize = 29,
    DbcQrDataWords = 44,
    DbcQrEcWords = 26,
    DbcQrTotalWords = 70,
    DbcQrMaxText = 42,
    DbcQrQuiet = 4,                /* 静区，模块数，标准要求至少 4 */
    DbcStampBits = 32,             /* 左上角的帧号条：每位一格，白 1 黑 0，低位在左 */
    DbcStampCell = 16,
    DbcScrollPixels = 6,           /* 每帧彩条左移的像素，偶数：色度是 2 像素一组 */
};

/* 测试图里的文本。URL 让代码扫描器给出明确的横幅；点了才联网，识别不需要网络。 */
#define DBC_QR_TEXT "https://unixnu.com/pagani-cam-test"

typedef struct {
    uint8_t module[DbcQrSize][DbcQrSize];     /* 1 黑 0 白，[行][列] */
} DbcQr;

/* GF(256) 上的 RS 余式（生成多项式 0x11d，根 α^0..α^25）。 */
void dbc_qr_ecc(const uint8_t data[DbcQrDataWords], uint8_t ecc[DbcQrEcWords]);
/* 0 或负数（文本太长、空）。mask 0..7。 */
int dbc_qr_encode(const char *text, size_t length, unsigned mask, DbcQr *out);

typedef struct {
    unsigned width, height;
    unsigned tile;              /* 二维码连同静区的边长，像素 */
    unsigned tile_left, tile_top;   /* 偶数 */
    uint8_t *line_y, *line_uv;  /* 两个周期的一行，平移时直接取 */
    uint8_t *tile_luma;         /* tile × tile */
} DbcPattern;

/* 宽高必须是正偶数，至少 64。0 或负数；负数时 p 不用释放。 */
int dbc_pattern_init(DbcPattern *p, unsigned width, unsigned height);
void dbc_pattern_free(DbcPattern *p);
/* 画第 frame 帧。y 至少 height 行、stride ≥ width；uv 至少 height / 2 行、stride ≥ width。 */
void dbc_pattern_fill(const DbcPattern *p, uint32_t frame, uint8_t *y, size_t y_stride, uint8_t *uv, size_t uv_stride);
/* 从画好的 Y 平面读回帧号（测试和取证用）。 */
uint32_t dbc_pattern_stamp(const uint8_t *y, size_t y_stride);

#ifdef __cplusplus
}
#endif
#endif
