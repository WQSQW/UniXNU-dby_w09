/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The portable torch core for DBY-W09: the register semantics of the flash-v2 module, the vendor's
 * on/off order, the current law, the thermal derate and the fault rules. No platform calls live here
 * (DbyTorchCore.c takes a DbtSpmiIo and a DbtOps), which is what makes Test/ able to run the whole
 * state machine on the host. Structure follows PaganiTorch's TorchCore.h; every number follows
 * leds-qpnp-flash-v2.c and this unit's stock DTBO - see DbyTorchRegs.h. */
#ifndef DBY_TORCH_CORE_H
#define DBY_TORCH_CORE_H

#include <stdint.h>

#include "DbyTorchSpmi.h"

enum {
    DBT_MODE_OFF = 0,     /* no boot argument at all: nothing is mapped, nothing is written */
    DBT_MODE_ON = 1,
    DBT_MODE_DRY = 2,     /* read identity and registers, write nothing, then stop */
};

enum {
    DBT_ST_OFF = 0,
    DBT_ST_ON = 1,
    DBT_ST_DRY = 2,
    DBT_ST_REFUSED = 3,
    DBT_ST_FAULT = 4,
};

typedef struct {
    unsigned mode;
    unsigned ee;
    uint32_t ppid;
    unsigned led_mask;    /* this board's only switch: channel 0 */
    unsigned torch_id;
    unsigned hard_ma;     /* qcom,max-current: the ceiling the gate will ever sign */
    unsigned max_ma;      /* qcom,current-ma: what level DBT_LEVEL_MAX means */
    unsigned ires_ua;     /* qcom,ires-ua: the resolution the vendor code starts from */
    unsigned auto_off_ms;
    unsigned poll_ms;
    unsigned log;         /* 0 none, 1 rationed (the default), 2 every event line */
} DbtConfig;

typedef struct {
    void *ctx;
    uint64_t (*now_us)(void *ctx);
    void (*trace)(void *ctx, const char *line);
} DbtOps;

typedef struct {
    DbtConfig cfg;
    DbtSpmi spmi;
    const DbtOps *ops;

    unsigned state;
    int stage;            /* 1 bus, 2 identity, 3 dump, 4 ready; negative = where it stopped */
    unsigned level;       /* 0..DBT_LEVEL_MAX, as the OS asked for it */
    unsigned ma;          /* what that became after the vendor's current law */
    unsigned code;        /* the byte programmed into TGR_CURRENT */
    unsigned ires_idx;
    const char *why;
    int last_rc;

    uint8_t sts[3];       /* STATUS1, STATUS2, INT_RT_STS as of the last read */
    uint8_t boot_sts[3];
    uint8_t mod_ctrl, ires, strobe, tgr, tmr, en_led, hdrm, thrsh[3];

    uint64_t on_us, deadline_us, event_line_us;
    unsigned ons, offs, faults, event_lines, dropped, derates;
    unsigned fault_lines;
    int hard_code;        /* code ceiling in force, lowered by the thermal derate */
    int code_max;         /* code of cfg.max_ma, the policy ceiling */
} DbtCore;

int dbt_config_make(DbtConfig *c, unsigned mode, unsigned ee, unsigned hard_ma, unsigned max_ma_arg,
                    unsigned log);
uint32_t dbt_test_ms(unsigned arg);
/* The vendor's current law, [V]:1339-1387 and [V]:374-380, PM8150L erratum included. */
void dbt_current_law(unsigned ma, unsigned default_ires_ua, unsigned hard_ma, unsigned *ires_idx,
                     unsigned *ires_ua, unsigned *code);
unsigned dbt_level_ma(unsigned level, unsigned max_ma);

int  dbt_core_init(DbtCore *t, const DbtConfig *c, const DbtOps *ops, const DbtSpmiIo *io);
int  dbt_core_set(DbtCore *t, unsigned level, uint32_t hold_ms, const char *why);
uint32_t dbt_core_poll(DbtCore *t);
void dbt_core_export(const DbtCore *t, int32_t out[16]);

#endif
