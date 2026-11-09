/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The log-line builder of the portable torch sources (no C library: the same files build into the
 * kernel and into the host tests), copied from PaganiTorch's TorchText.h with this module's prefix.
 * It never writes past its buffer and always leaves the string terminated. */
#ifndef DBY_TORCH_TEXT_H
#define DBY_TORCH_TEXT_H

#include <stddef.h>
#include <stdint.h>

static inline void dbt_zero(void *to, size_t n)
{
    uint8_t *d = (uint8_t *)to;
    for (size_t i = 0; i < n; ++i) d[i] = 0;
}

typedef struct {
    char *buf;
    size_t size, used;
} DbtText;

static inline void dbt_text_start(DbtText *t, char *buf, size_t size)
{
    t->buf = buf;
    t->size = size;
    t->used = 0;
    if (size) buf[0] = 0;
}
static inline void dbt_text_char(DbtText *t, char c)
{
    if (t->used + 1 < t->size) {
        t->buf[t->used++] = c;
        t->buf[t->used] = 0;
    }
}
static inline void dbt_text_str(DbtText *t, const char *s)
{
    for (size_t i = 0; s && s[i]; ++i) dbt_text_char(t, s[i]);
}
static inline void dbt_text_udec(DbtText *t, uint64_t v)
{
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < sizeof digits);
    while (n) dbt_text_char(t, digits[--n]);
}
static inline void dbt_text_dec(DbtText *t, int64_t v)
{
    if (v < 0) {
        dbt_text_char(t, '-');
        dbt_text_udec(t, (uint64_t)(-(v + 1)) + 1);
    } else {
        dbt_text_udec(t, (uint64_t)v);
    }
}
static inline void dbt_text_hex2(DbtText *t, unsigned v)
{
    static const char hex[] = "0123456789abcdef";
    dbt_text_char(t, hex[(v >> 4) & 15]);
    dbt_text_char(t, hex[v & 15]);
}
static inline void dbt_text_hex(DbtText *t, uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    char digits[16];
    unsigned n = 0;
    dbt_text_str(t, "0x");
    do {
        digits[n++] = hex[v & 15];
        v >>= 4;
    } while (v && n < sizeof digits);
    while (n) dbt_text_char(t, digits[--n]);
}
/* " key=value" fields */
static inline void dbt_text_kd(DbtText *t, const char *key, int64_t v)
{
    dbt_text_char(t, ' ');
    dbt_text_str(t, key);
    dbt_text_char(t, '=');
    dbt_text_dec(t, v);
}
static inline void dbt_text_kx(DbtText *t, const char *key, uint64_t v)
{
    dbt_text_char(t, ' ');
    dbt_text_str(t, key);
    dbt_text_char(t, '=');
    dbt_text_hex(t, v);
}
static inline void dbt_text_ks(DbtText *t, const char *key, const char *v)
{
    dbt_text_char(t, ' ');
    dbt_text_str(t, key);
    dbt_text_char(t, '=');
    dbt_text_str(t, v);
}

#endif
