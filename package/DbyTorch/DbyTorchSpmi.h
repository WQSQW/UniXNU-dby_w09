/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The SPMI side of the torch (DbyTorchSpmi.c): one PMIC peripheral - PM8150L's flash block at PPID
 * 0x5d3 - reached through the SM8250 v5 arbiter, plus the write gate in front of it. Facts and
 * sources in DbyTorchRegs.h ([V] = vendor leds-qpnp-flash-v2.c, [D] = this unit's stock DTBO). */
#ifndef DBY_TORCH_SPMI_H
#define DBY_TORCH_SPMI_H

#include <stdint.h>

#include "DbyTorchRegs.h"

/* Register access is supplied by the caller (the kernel maps the windows; the host tests emulate
 * them), so this file holds no platform code at all. Read side: the observer channel of this EE.
 * Write side: the request channel page of the flash APID, mapped once by map_channel. */
typedef struct {
    void *ctx;
    uint32_t (*core)(void *ctx, uint32_t at);
    uint32_t (*cnfg)(void *ctx, uint32_t at);
    /* this EE's observer region, offset = DBT_ARB_OBS_APID_STRIDE * apid + register */
    uint32_t (*obs)(void *ctx, uint32_t at);
    void (*obs_put)(void *ctx, uint32_t at, uint32_t value);
    int (*chan_map)(void *ctx, unsigned apid);     /* 0 once the RW page of that APID is reachable */
    uint32_t (*chan)(void *ctx, uint32_t at);
    void (*chan_put)(void *ctx, uint32_t at, uint32_t value);
    void (*delay)(void *ctx, unsigned us);
} DbtSpmiIo;

/* A command-word shape that answered, and the three that may. Ladder memory is per direction: the
 * read side and the write side of this arbiter answer to different opcodes on this board. */
typedef struct {
    uint32_t opcode;
    uint8_t with_sid;
} DbtOpForm;

typedef struct {
    const DbtSpmiIo *io;
    unsigned ee;
    uint32_t version;
    int apid;                 /* the flash channel's APID, -1 until the table answers */
    int owner;                /* write EE of that channel, -1 unknown */
    unsigned irq_owner;
    unsigned slots;           /* APID table entries actually read */
    unsigned ppid_hits;       /* how many of them claim PPID 0x5d3 */
    uint32_t last_reg, last_status;

    uint64_t reads, writes, retries, timeouts, errors, refused, denied;
    int rform, wform;         /* -2 not settled, -1 every shape failed, 0..2 the shape that works */
    int silent;               /* a transaction stopped answering at all: end the cycle */
    int closed;               /* gate or bus failure: nothing is written until reboot */
    int rw_mapped;

    /* The interlock: the channel may only be switched on once this gate itself has written every
     * field that decides what the light does at that current. Each is the last value accepted. */
    uint8_t w_ires, w_current, w_timer, w_strobe;
    uint8_t have_ires, have_current, have_timer, have_strobe;
    unsigned hard_code;       /* the current ceiling the gate will sign, re-derived by the core */
} DbtSpmi;

/* Why a call stopped, kept distinct because the vendor keeps them distinct (spmi-pmic-arb.c
 * pmic_arb_wait_for_done: a timeout is not an error bit, and a DENIED error is not a bad address).
 * The numbers are Linux's own errnos, which is what SPMI answers with on this board (DbyBattery.cpp
 * reads -ENODEV and -ETIMEDOUT out of the same calls), and they are deliberately all different: two
 * of these sharing -1 would make the gate's refusal and a silent arbiter one event to read. */
enum {
    DBT_OK = 0,
    DBT_PERM = -1,      /* -EPERM: the gate would not sign this byte, so nothing was sent */
    DBT_ERROR = -5,     /* -EIO: DONE with FAILURE/DENIED/DROPPED, on every shape the ladder tried */
    DBT_NODEV = -19,    /* -ENODEV: no channel for this PPID, and no bus to write through */
    DBT_TIMEOUT = -110, /* -ETIMEDOUT: a transaction that never signalled DONE */
};

int  dbt_spmi_discover(DbtSpmi *s, const DbtSpmiIo *io, unsigned ee);
int  dbt_spmi_open(DbtSpmi *s, unsigned hard_code);
int  dbt_spmi_read(DbtSpmi *s, uint8_t addr, uint8_t *out);
int  dbt_spmi_write(DbtSpmi *s, uint8_t addr, uint8_t mask, uint8_t value);
/* The one emergency stop: EN_LED_CTRL = 0 for this board's led-mask, MOD_CTRL off. Allowed once even
 * on a closed gate, because leaving the light on is the only outcome that must not happen. */
int  dbt_spmi_emergency_stop(DbtSpmi *s);
int  dbt_spmi_allowed(const DbtSpmi *s, uint8_t addr, uint8_t mask, uint8_t value);
void dbt_spmi_reset_interlock(DbtSpmi *s);

#endif
