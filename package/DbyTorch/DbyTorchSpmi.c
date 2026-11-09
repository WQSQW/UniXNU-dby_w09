/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The SPMI engine and the write gate (DbyTorchSpmi.h). Nothing here knows about XNU: the windows are
 * behind DbtSpmiIo, so the host tests in Test/ run this exact file against a model of the arbiter.
 *
 * Why a gate at all: one wrong byte on this block keeps the LED on with nothing left to turn it off,
 * and the software has no IRQ handler and no second owner. So every write is checked against the
 * vendor's own torch path ([V] qpnp_flash_led_switch_set / _switch_disable, the only two functions
 * that ever touch these seven bytes), the channel may only light after this gate has itself written
 * the current, the timer, the resolution and the strobe selection, and a failure closes the bus. */
#include "DbyTorchSpmi.h"
#include "DbyTorchText.h"

static const DbtOpForm kReadForms[DBT_FORM_COUNT] = DBT_READ_FORMS;
static const DbtOpForm kWriteForms[DBT_FORM_COUNT] = DBT_WRITE_FORMS;

#define OBS(at) (s->io->obs(s->io->ctx, (at)))
#define OBS_PUT(at, v) (s->io->obs_put(s->io->ctx, (at), (v)))
#define CHN(at) (s->io->chan(s->io->ctx, (at)))
#define CHN_PUT(at, v) (s->io->chan_put(s->io->ctx, (at), (v)))

static uint32_t cmd_word(const DbtOpForm *f, uint8_t addr)
{
    uint32_t c = (f->opcode << DBT_CMD_OP_SHIFT) | ((uint32_t)addr << DBT_CMD_ADDR_SHIFT);
    if (f->with_sid) c |= (uint32_t)DBT_USID << DBT_CMD_SID_SHIFT;
    return c;   /* one byte: len-1 == 0, so there is no length term ([V] uses u8 everywhere) */
}

/* One observer transaction. 1 = DONE clean with data, 0 = never DONE, -1 = DONE with an error bit -
 * the three cases spmi-pmic-arb.c pmic_arb_wait_for_done() keeps apart, and the caller must too. */
static int xact_read(DbtSpmi *s, unsigned form, uint8_t addr, uint8_t *out)
{
    const uint32_t base = (uint32_t)DBT_ARB_OBS_APID_STRIDE * (uint32_t)s->apid;
    OBS_PUT(base + DBT_ARB_CMD, cmd_word(&kReadForms[form], addr));
    uint32_t status = 0;
    for (unsigned i = 0; i < DBT_POLL_COUNT; ++i) {
        status = OBS(base + DBT_ARB_STATUS);
        if (status & DBT_STS_DONE) break;
        if (s->io->delay) s->io->delay(s->io->ctx, 1u);
    }
    s->last_status = status;
    if (!(status & DBT_STS_DONE)) return 0;
    if (status & DBT_STS_ERROR_MASK) return -1;
    *out = (uint8_t)(OBS(base + DBT_ARB_RDATA0) & 0xFFu);
    return 1;
}

/* One request-channel transaction, in the vendor's order: WDATA0 first, then the command word, then
 * DONE on that same channel (spmi-pmic-arb.c pmic_arb_write_cmd, DbyBattery.cpp's accepted shape). */
static int xact_write(DbtSpmi *s, unsigned form, uint8_t addr, uint8_t value)
{
    if (!s->rw_mapped) return 0;
    CHN_PUT(DBT_ARB_WDATA0, value);
    CHN_PUT(DBT_ARB_CMD, cmd_word(&kWriteForms[form], addr));
    uint32_t status = 0;
    for (unsigned i = 0; i < DBT_POLL_COUNT; ++i) {
        status = CHN(DBT_ARB_STATUS);
        if (status & DBT_STS_DONE) break;
        if (s->io->delay) s->io->delay(s->io->ctx, 1u);
    }
    s->last_status = status;
    if (!(status & DBT_STS_DONE)) return 0;
    if (status & DBT_STS_ERROR_MASK) return -1;
    return 1;
}

/* The ladder, in both directions: a remembered shape is retried first, a shape that comes back with
 * an error bit goes the whole ladder again, and a shape that never answers ends the cycle instead of
 * costing every later call a poll each. `byte` is the data in on a write, the result out on a read. */
static int laddered(DbtSpmi *s, uint8_t addr, uint8_t *byte, int write, uint64_t *counter)
{
    s->last_reg = addr;
    int *form = write ? &s->wform : &s->rform;
    if (*form >= 0) {
        const unsigned f = (unsigned)*form;
        const int r = write ? xact_write(s, f, addr, *byte) : xact_read(s, f, addr, byte);
        if (r == 1) { ++*counter; return DBT_OK; }
        if (r == 0) { s->silent = 1; return DBT_TIMEOUT; }
        ++s->errors;
        *form = -2;
        ++s->retries;
    }
    for (unsigned i = 0; i < DBT_FORM_COUNT; ++i) {
        const int r = write ? xact_write(s, i, addr, *byte) : xact_read(s, i, addr, byte);
        if (r == 1) {
            *form = (int)i;
            ++*counter;
            return DBT_OK;
        }
        if (r == 0) { s->silent = 1; return DBT_TIMEOUT; }
        ++s->errors;
        ++s->retries;
    }
    return DBT_ERROR;
}

int dbt_spmi_read(DbtSpmi *s, uint8_t addr, uint8_t *out)
{
    if (!s || !s->io || !out) return DBT_PERM;
    if (s->apid < 0) return DBT_NODEV;
    if (s->silent) return DBT_TIMEOUT;
    return laddered(s, addr, out, 0, &s->reads);
}

/* The gate. `mask` and `value` are exactly the two arguments the vendor's masked_write takes, so a
 * refusal here means "leds-qpnp-flash-v2.c never issues this write on this board". */
int dbt_spmi_allowed(const DbtSpmi *s, uint8_t addr, uint8_t mask, uint8_t value)
{
    if (s->closed) return 0;
    switch (addr) {
    case DBT_R_EN_LED_CTRL:
        /* [V]:1764 masked_write(base, snode->led_mask, val) - and this board declares one switch,
         * led_switch_0, whose qcom,led-mask is 0x1. Anything wider is not this board's light. */
        if (mask != DBT_LED_MASK) return 0;
        if (value & (uint8_t)~DBT_LED_MASK) return 0;
        if (value & DBT_LED_MASK)
            return s->have_ires && s->have_current && s->have_timer && s->have_strobe;
        return 1;
    case DBT_R_MOD_CTRL:
        return mask == DBT_M_MOD_CTRL && !(value & (uint8_t)~DBT_M_MOD_CTRL);
    case DBT_R_IRES:
        /* [V]:1662 masked_write(IRES, FLASH_LED_CURRENT_MASK, ires_idx << (id * 2)). Channel 0 owns
         * bits 1:0 only, and ires_idx is 0..3 by [V]:2214 and [V]:1358-1370. */
        return mask == DBT_M_CURRENT && value < DBT_IRES_LEVELS;
    case DBT_R_STROBE_CTRL:
        /* [V]:1690-1696: with SW_STROBE the mask is BIT(2) alone and the value is strobe_ctrl, which
         * [V]:2093 leaves at 0 for a software-strobed torch. */
        return mask == DBT_M_STROBE_SEL && !(value & DBT_M_STROBE_SEL);
    case DBT_R_TGR_CURRENT:
        return mask == DBT_M_CURRENT && value <= s->hard_code;
    case DBT_R_SAFETY_TMR:
        /* [V]:1706 qpnp_flash_led_write(SAFETY_TMR, fnode.duration) - a whole byte, and torch_0
         * carries no qcom,duration-ms, so the one value this board ever writes is the disabled 0x13. */
        return mask == 0xFFu && value == (uint8_t)DBT_TORCH_TMR;
    default:
        return 0;
    }
}

int dbt_spmi_write(DbtSpmi *s, uint8_t addr, uint8_t mask, uint8_t value)
{
    if (!s || !s->io) return DBT_PERM;
    if (s->apid < 0 || !s->rw_mapped) return DBT_NODEV;
    if (s->silent) return DBT_TIMEOUT;
    if (!dbt_spmi_allowed(s, addr, mask, value)) {
        ++s->refused;
        return DBT_PERM;
    }
    /* regmap_update_bits semantics ([V]:427-437): read the byte, merge, write the byte back. */
    uint8_t cur = 0;
    int rc = dbt_spmi_read(s, addr, &cur);
    if (rc) return rc;
    const uint8_t wire = (uint8_t)((cur & ~mask) | (value & mask));
    uint8_t payload = wire;
    rc = laddered(s, addr, &payload, 1, &s->writes);
    if (rc) return rc;
    switch (addr) {
    case DBT_R_IRES:        s->w_ires = value;        s->have_ires = 1; break;
    case DBT_R_TGR_CURRENT: s->w_current = value;     s->have_current = 1; break;
    case DBT_R_SAFETY_TMR:  s->w_timer = value;       s->have_timer = 1; break;
    case DBT_R_STROBE_CTRL: s->w_strobe = value;      s->have_strobe = 1; break;
    case DBT_R_EN_LED_CTRL:
        if (!(value & DBT_LED_MASK)) dbt_spmi_reset_interlock(s);
        break;
    default: break;
    }
    return DBT_OK;
}

void dbt_spmi_reset_interlock(DbtSpmi *s)
{
    s->w_ires = s->w_current = s->w_timer = s->w_strobe = 0;
    s->have_ires = s->have_current = s->have_timer = s->have_strobe = 0;
}

/* Off, whatever the state of the gate, and only ever once: [V] puts the channels away before it
 * touches anything else (qpnp_flash_led_switch_disable) and that is the order kept here. */
int dbt_spmi_emergency_stop(DbtSpmi *s)
{
    if (!s || !s->io || s->apid < 0) return DBT_NODEV;
    /* Two writers would be a race; there is one, the pagani-style torch thread, and every register
     * access in this port happens on it. */
    s->closed = 0;                       /* the two writes below are the gate's one exception */
    const int a = dbt_spmi_write(s, DBT_R_EN_LED_CTRL, DBT_LED_MASK, 0);
    const int b = dbt_spmi_write(s, DBT_R_MOD_CTRL, DBT_M_MOD_CTRL, DBT_V_MOD_DISABLE);
    s->closed = 1;                       /* and nothing is written again until reboot */
    dbt_spmi_reset_interlock(s);
    if (a) return a;
    if (b) return b;
    return DBT_OK;
}

/* ---- bring-up of the bus itself ---- */

int dbt_spmi_discover(DbtSpmi *s, const DbtSpmiIo *io, unsigned ee)
{
    if (!s || !io || !io->core || !io->obs || !io->obs_put) return DBT_PERM;
    s->io = io;
    s->ee = ee;
    s->apid = -1;
    s->owner = -1;
    /* neither ladder has settled yet: zero would mean "shape 0 is known to work", which is a claim
     * this call has no evidence for and which would skip the first two spellings forever */
    s->rform = s->wform = -2;

    const uint32_t version = io->core(io->ctx, DBT_ARB_REG_VERSION);
    s->version = version;
    if (version < DBT_ARB_V5_MIN || version >= DBT_ARB_V5_MAX) return DBT_NODEV;

    unsigned entries = (DBT_ARB_CORE_SZ - DBT_ARB_APID_MAP) / 4u;
    if (entries > DBT_ARB_MAX_APIDS) entries = DBT_ARB_MAX_APIDS;
    unsigned best = 0, best_owner = 0xFFu;
    int have_best = 0, have_ours = 0;
    for (unsigned i = 0; i < entries; ++i) {
        const uint32_t map = io->core(io->ctx, DBT_ARB_APID_MAP + 4u * i);
        if (!map) continue;                       /* pmic_arb_read_apid_map_v5 skips zero cells */
        ++s->slots;
        if (((map >> DBT_ARB_PPID_SHIFT) & DBT_ARB_PPID_MASK) != DBT_PPID) continue;
        ++s->ppid_hits;
        /* 0xFF = "the table did not answer": an unknown owner is never treated as this EE's. */
        const unsigned owner = io->cnfg ? (io->cnfg(io->ctx, DBT_ARB_OWNERSHIP + 4u * i)
                                          & DBT_ARB_OWNER_MASK) : 0xFFu;
        if (!have_best) { best = i; best_owner = owner; have_best = 1; }
        if (owner == ee && !have_ours) {
            best = i; best_owner = owner; have_ours = 1;
            s->irq_owner = (map & DBT_ARB_IRQ_OWNER_BIT) ? 1u : 0u;
        }
    }
    if (!have_best) return DBT_NODEV;
    s->apid = (int)best;
    s->owner = (int)best_owner;
    return DBT_OK;
}

int dbt_spmi_open(DbtSpmi *s, unsigned hard_code)
{
    if (!s || !s->io) return DBT_PERM;
    if (s->apid < 0) return DBT_NODEV;
    s->hard_code = hard_code > 0x7Fu ? 0x7Fu : hard_code;
    /* spmi-pmic-arb.c will not issue a write on a channel this EE does not own (-EPERM at :1007-1011)
     * and neither will this file: the ownership table is consulted before the page is ever mapped. */
    if (s->owner != (int)s->ee) { ++s->denied; return DBT_PERM; }
    if (!s->io->chan_map || !s->io->chan || !s->io->chan_put) return DBT_NODEV;
    /* No status probe here: the DONE bit of a request channel only means anything after a
     * transaction, so a healthy idle channel reads 0 and a dead window reads 0 too. The glue's own
     * map result is the only honest answer, and the first real transaction settles the ladder. */
    if (s->io->chan_map(s->io->ctx, (unsigned)s->apid) != 0) return DBT_NODEV;
    s->rw_mapped = 1;
    return DBT_OK;
}
