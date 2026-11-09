/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The torch core (DbyTorchCore.h). [V] = leds-qpnp-flash-v2.c, [D] = this unit's stock DTBO.
 *
 * Torch on, in [V] qpnp_flash_led_switch_set()'s order with clear-first and read-back checks added:
 * the channel off if it was on, the module off if it was on, IRES, the channel's
 * STROBE_CTRL, the channel's TGR_CURRENT, the channel's SAFETY_TMR, then all of it read back, then
 * MOD_CTRL BIT7 (qpnp_flash_led_module_enable), then EN_LED_CTRL (the gate signs it only because it
 * wrote every one of those four itself). Torch off, in [V] qpnp_flash_led_switch_disable()'s order:
 * EN_LED_CTRL, MOD_CTRL, TGR_CURRENT 0, SAFETY_TMR 0x13, and the read-back that says it stayed off.
 *
 * The light is derated, not stopped, by heat, because the vendor says so: [D] sets
 * qcom,thermal-derate-en with qcom,thermal-derate-current = <200 500 1000> and [V]:1290-1318 feeds
 * that table to min(max_avail_current, thermal_current_lim) - which for a torch is a reprogram of
 * TGR_CURRENT alone, the branch [V]:1667-1681 already exists for. STATUS1 non-zero is short/open and
 * STATUS2 BIT(4) a VPH droop, and [V] reads both in one place only: its led-fault-irq branch
 * ([V]:1992-2013), the line the board DTs put at INT_RT_STS bit 0. With that line clear the two status
 * bytes are evidence, not a stop, and a lamp fault goes through the ordinary off ladder so the next
 * request can light again; after a bus failure, refusal, timeout or failed read-back the bus closes
 * and stays closed. */
#include "DbyTorchCore.h"
#include "DbyTorchText.h"

enum {
    LINE = 200,
    EVENT_FIRST = 8,            /* log 1: the first on/off lines, then one per EVENT_GAP_US */
    EVENT_MAX_RATIONED = 24, EVENT_MAX_ALL = 64, FAULT_LINES = 3,
    TEST_MAX_MS = 1000,
};
#define EVENT_GAP_US 300000000ull

static uint64_t now_us(const DbtCore *t) {return t->ops->now_us(t->ops->ctx);}
static void trace(const DbtCore *t, const char *line)
{
    if (t->ops->trace) t->ops->trace(t->ops->ctx, line);
}
static void begin(DbtText *x, char *buf)
{
    dbt_text_start(x, buf, LINE);
    dbt_text_str(x, "[torch] ");
}

static int rd(DbtCore *t, uint8_t addr, uint8_t *v) {return dbt_spmi_read(&t->spmi, addr, v);}
static int wr(DbtCore *t, uint8_t addr, uint8_t mask, uint8_t v)
{
    return dbt_spmi_write(&t->spmi, addr, mask, v);
}

/* ---- the configuration ---- */
int dbt_config_make(DbtConfig *c, unsigned mode, unsigned ee, unsigned hard_ma, unsigned max_ma_arg,
                    unsigned log)
{
    if (!c || (mode != DBT_MODE_ON && mode != DBT_MODE_DRY)) return 0;
    dbt_zero(c, sizeof *c);
    /* [D] qcom,max-current = 0x1f4 is the module's own ceiling for a torch and [D]
     * qcom,current-ma = 0x12c is what the vendor programs at full brightness; a boot argument may
     * only come down from those two, never up - and a missing one is that cell, not the other. */
    if (hard_ma > DBT_TORCH_MAX_MA) hard_ma = DBT_TORCH_MAX_MA;
    if (!hard_ma) hard_ma = DBT_TORCH_MAX_MA;
    unsigned cap = max_ma_arg ? max_ma_arg : DBT_TORCH_MA;
    if (cap > hard_ma) cap = hard_ma;
    c->mode = mode;
    c->ee = ee;
    c->ppid = DBT_PPID;
    c->led_mask = DBT_LED_MASK;
    c->torch_id = DBT_TORCH_ID;
    c->hard_ma = hard_ma;
    c->max_ma = cap;
    c->ires_ua = DBT_TORCH_IRES_UA;
    c->auto_off_ms = DBT_AUTO_OFF_MS;
    c->poll_ms = DBT_POLL_DEFAULT_MS;
    c->log = (unsigned)(log > 2 ? 2 : log);
    return 1;
}

uint32_t dbt_test_ms(unsigned arg) {return arg < TEST_MAX_MS ? arg : TEST_MAX_MS;}

/* [V]:374-380 and [V]:1339-1387, PM8150L_IRES_WA included. The two numbers the hardware ends up
 * needing are the resolution index (which goes into IRES) and the code (which goes into
 * TGR_CURRENT); the cap is [V]:1355's min(prgm, max_current). */
void dbt_current_law(unsigned ma, unsigned default_ires_ua, unsigned hard_ma, unsigned *ires_idx,
                     unsigned *ires_ua, unsigned *code)
{
    static const unsigned max_of_ires[DBT_IRES_LEVELS] = DBT_IRES_MAX_MA;
    unsigned prgm = ma;
    const unsigned min_ma = default_ires_ua / 1000u;
    if (prgm && prgm < min_ma) prgm = min_ma;
    if (prgm > hard_ma) prgm = hard_ma;

    unsigned idx = DBT_IRES_DEFAULT_IDX, ires = default_ires_ua;
    if (prgm > max_of_ires[idx]) {
        for (int i = (int)DBT_IRES_LEVELS - 1; i >= 0; --i) {
            if (prgm <= max_of_ires[(unsigned)i]) {
                idx = (unsigned)i;
                ires = DBT_IRES_MIN_UA + (DBT_IRES_BASE - idx) * DBT_IRES_DIVISOR;
                break;
            }
        }
    } else if (prgm && prgm <= DBT_WA_MAX_MA) {
        idx = DBT_IRES_BASE;                        /* [V]:1367-1370, the erratum branch */
        ires = DBT_IRES_MIN_UA;
    }

    unsigned c = 0;
    if (prgm && ires && prgm >= ires / 1000u)
        c = (prgm * 1000u + ires / 2u) / ires - 1u; /* DIV_ROUND_CLOSEST(prgm*1000, ires) - 1 */
    if (c > DBT_M_CURRENT) c = DBT_M_CURRENT;
    if (ires_idx) *ires_idx = idx;
    if (ires_ua) *ires_ua = ires;
    if (code) *code = c;
}

/* The OS hands a level down, the vendor's DT says what full means in milliamps, and the floor is the
 * vendor's own: below one resolution step the current is the step ([V]:2238). */
unsigned dbt_level_ma(unsigned level, unsigned max_ma)
{
    if (!level) return 0;
    if (level > DBT_LEVEL_MAX) level = DBT_LEVEL_MAX;
    const unsigned min_ma = DBT_TORCH_IRES_UA / 1000u;
    const unsigned ma = (unsigned)(((uint64_t)level * max_ma + DBT_LEVEL_MAX / 2) / DBT_LEVEL_MAX);
    return ma < min_ma ? min_ma : ma;
}

/* ---- the log ---- */
static int event_line_allowed(DbtCore *t)
{
    if (!t->cfg.log) { ++t->dropped; return 0; }
    const uint64_t n = now_us(t);
    if (t->event_lines >= (t->cfg.log >= 2 ? EVENT_MAX_ALL : EVENT_MAX_RATIONED) ||
        (t->cfg.log == 1 && t->event_lines >= EVENT_FIRST && n - t->event_line_us < EVENT_GAP_US)) {
        ++t->dropped;
        return 0;
    }
    ++t->event_lines;
    t->event_line_us = n;
    return 1;
}

static void status_fields(DbtText *x, const uint8_t s[3])
{
    dbt_text_str(x, " s1=");
    dbt_text_hex2(x, s[0]);
    dbt_text_str(x, " s2=");
    dbt_text_hex2(x, s[1]);
    dbt_text_str(x, " rt=");
    dbt_text_hex2(x, s[2]);
}

/* ---- faults and derate ---- */
/* The vendor's two hard faults, and only its two: STATUS1 any bit is short/open ([V]:2007-2009) and
 * STATUS2 BIT(4) is the VPH droop ([V]:34,2011, and [D] enables qcom,vph-droop-det). Thermal OTST is
 * deliberately absent - it derates here, it does not stop.
 * [V] reads both bytes in one branch only: led-fault-irq ([V]:1992), which every recorded board's DT
 * puts at INT_RT_STS bit 0. Reading them while that line is clear is not the vendor's mechanism, and
 * ord214 paid for the difference: the module's ramp-up left STATUS1=01 with the fault line clear and
 * this port called a working light a short/open. */
static const char *fault_of(const DbtCore *t)
{
    if (!(t->sts[2] & DBT_V_LED_FAULT)) return 0;
    if (t->sts[0]) return "short-or-open";
    if (t->sts[1] & DBT_V_VPH_DROOP) return "vph-droop";
    return 0;
}

static unsigned thermal_limit_ma(uint8_t status2)
{
    const unsigned otst = (unsigned)(status2 & DBT_M_OTST);
    if (!otst) return 0;
    static const unsigned derate[3] = DBT_DERATE_MA;
    unsigned i = otst >> 1;
    if (i > DBT_DERATE_LAST) i = DBT_DERATE_LAST;   /* [V] would read one past the DT's three cells */
    return derate[i];
}

/* Stops the LED (the gate's one emergency stop) and keeps the torch off until reboot. */
static int fault(DbtCore *t, const char *why, int rc)
{
    const int stop = t->spmi.rw_mapped ? dbt_spmi_emergency_stop(&t->spmi) : DBT_NODEV;
    t->state = DBT_ST_FAULT;
    ++t->faults;
    t->level = 0;
    t->ma = 0;
    t->code = 0;
    t->why = why;
    t->last_rc = rc;
    if (t->fault_lines < FAULT_LINES) {
        ++t->fault_lines;
        char buf[LINE];
        DbtText x;
        begin(&x, buf);
        dbt_text_str(&x, "FAULT ");
        dbt_text_str(&x, why);
        dbt_text_kd(&x, "rc", rc);
        dbt_text_kd(&x, "stop", stop);
        status_fields(&x, t->sts);
        dbt_text_kx(&x, "reg", t->spmi.last_reg);
        dbt_text_kx(&x, "status", t->spmi.last_status);
        dbt_text_kd(&x, "refused", (int64_t)t->spmi.refused);
        dbt_text_str(&x, ": torch off until reboot");
        trace(t, buf);
    }
    return -5;
}

static int torch_off(DbtCore *t);

/* A lamp fault is [V]'s fault branch, and [V] touches nothing there but the log: the module has
 * already stopped itself. So this puts the light away with the accepted switch_disable ladder and
 * leaves the gate open, which lets the next request re-run the light-up. Keeping the driver shut
 * until reboot was this port's own rule, and it is what made the switch dead for the whole boot; a
 * bus failure is a different thing - the windows are gone - and still latches through fault(). */
static void lamp_fault(DbtCore *t, const char *why)
{
    const int rc = torch_off(t);
    ++t->faults;
    t->why = why;
    t->last_rc = -5;
    if (rc == DBT_OK) ++t->offs;
    if (t->fault_lines < FAULT_LINES) {
        ++t->fault_lines;
        char buf[LINE];
        DbtText x;
        begin(&x, buf);
        dbt_text_str(&x, "FAULT ");
        dbt_text_str(&x, why);
        dbt_text_kd(&x, "rc", -5);
        dbt_text_kd(&x, "off", rc);
        status_fields(&x, t->sts);
        dbt_text_kx(&x, "reg", t->spmi.last_reg);
        dbt_text_kx(&x, "status", t->spmi.last_status);
        dbt_text_kd(&x, "refused", (int64_t)t->spmi.refused);
        dbt_text_str(&x, ": off, and the next request lights it again");
        trace(t, buf);
    }
}

/* STATUS1, STATUS2, INT_RT_STS. Three one-byte transactions: this arbiter's accepted shape on this
 * board (DbyBattery.cpp sends one byte per command word) and [V] asks for one byte per register. */
static int read_status(DbtCore *t, uint8_t s[3])
{
    const int a = rd(t, DBT_R_STATUS1, &s[0]);
    if (a) return a;
    const int b = rd(t, DBT_R_STATUS2, &s[1]);
    if (b) return b;
    return rd(t, DBT_R_INT_RT_STS, &s[2]);
}

/* ---- bring-up ---- */
static int stop_at(DbtCore *t, int stage, const char *why, int rc)
{
    t->stage = stage;
    t->state = DBT_ST_REFUSED;
    t->why = why;
    t->last_rc = rc;
    char buf[LINE];
    DbtText x;
    begin(&x, buf);
    dbt_text_str(&x, "refused: ");
    dbt_text_str(&x, why);
    dbt_text_kd(&x, "rc", rc);
    dbt_text_str(&x, " (nothing more is written; torch off)");
    trace(t, buf);
    return rc ? rc : DBT_NODEV;
}

static void spmi_line(DbtCore *t, int rc)
{
    char buf[LINE];
    DbtText x;
    begin(&x, buf);
    dbt_text_str(&x, "spmi");
    dbt_text_kd(&x, "rc", rc);
    dbt_text_kx(&x, "v", t->spmi.version);
    dbt_text_kd(&x, "apid", t->spmi.apid);
    dbt_text_kd(&x, "ee", (int64_t)t->cfg.ee);
    dbt_text_kd(&x, "own", t->spmi.owner);
    dbt_text_kd(&x, "slots", (int64_t)t->spmi.slots);
    dbt_text_kd(&x, "hits", (int64_t)t->spmi.ppid_hits);
    trace(t, buf);
}

/* The identity bytes and then the module's whole torch-facing register set, both as evidence lines.
 * The v2 driver validates no TYPE byte ([V]:29 defines only PERPH_SUBTYPE), so identity here is the
 * arbiter's own PPID 0x5d3 from [D] plus these reads; nothing is refused on a subtype number that
 * leds-qpnp-flash-v2.c itself never compares against anything. */
static int identity_and_dump(DbtCore *t)
{
    char buf[LINE];
    DbtText x;
    uint8_t type = 0, subtype = 0;
    int rc = rd(t, DBT_R_TYPE, &type);
    if (!rc) rc = rd(t, DBT_R_SUBTYPE, &subtype);
    begin(&x, buf);
    dbt_text_str(&x, "id flash=");
    dbt_text_hex2(&x, type);
    dbt_text_char(&x, '/');
    dbt_text_hex2(&x, subtype);
    dbt_text_kx(&x, "ppid", DBT_PPID);
    if (rc) dbt_text_kd(&x, "rc", rc);
    trace(t, buf);
    if (rc) return rc;

    static const uint8_t regs[] = {DBT_R_STATUS1, DBT_R_STATUS2, DBT_R_INT_RT_STS, DBT_R_SAFETY_TMR,
                                   DBT_R_TGR_CURRENT, DBT_R_MOD_CTRL, DBT_R_IRES, DBT_R_STROBE_CFG,
                                   DBT_R_STROBE_CTRL, DBT_R_EN_LED_CTRL, DBT_R_HDRM_PRGM,
                                   DBT_R_THRSH1, DBT_R_THRSH2, DBT_R_THRSH3};
    uint8_t d[sizeof regs];
    dbt_zero(d, sizeof d);
    begin(&x, buf);
    dbt_text_str(&x, "reg");
    for (unsigned i = 0; i < sizeof regs; ++i) {
        rc = rd(t, regs[i], &d[i]);
        if (rc) {
            dbt_text_kd(&x, "rc", rc);
            dbt_text_kx(&x, "at", regs[i]);
            break;
        }
        dbt_text_char(&x, ' ');
        dbt_text_hex2(&x, regs[i]);
        dbt_text_char(&x, '=');
        dbt_text_hex2(&x, d[i]);
    }
    t->boot_sts[0] = d[0];
    t->boot_sts[1] = d[1];
    t->boot_sts[2] = d[2];
    t->tmr = d[3];
    t->tgr = d[4];
    t->mod_ctrl = d[5];
    t->ires = d[6];
    t->strobe = d[8];
    t->en_led = d[9];
    t->hdrm = d[10];
    t->thrsh[0] = d[11];
    t->thrsh[1] = d[12];
    t->thrsh[2] = d[13];
    trace(t, buf);
    return rc;
}

/* The stages dbt_core_init records in t->stage, positive for "this far got done":
 *   1 the configuration is legal, 2 the arbiter named the channel, 3 the channel is ours to write,
 *   4 the light is known off, 5 the module's own bytes were read as evidence (and, for a dry run,
 *   that is as far as it goes). Negative is a stop before any write was possible: -1 config,
 *   -2 arbiter, -3 the dry run's read, -4 open. A failure after stage 3 is a fault, not a stop: the
 *   light is at stake, so the bus is closed and the reason is printed. */
int dbt_core_init(DbtCore *t, const DbtConfig *c, const DbtOps *ops, const DbtSpmiIo *io)
{
    if (!t || !c || !ops || !ops->now_us || !io) return -22;
    dbt_zero(t, sizeof *t);
    t->cfg = *c;
    t->ops = ops;
    t->stage = 1;
    if (c->mode != DBT_MODE_ON && c->mode != DBT_MODE_DRY) return stop_at(t, -1, "config", -22);
    if (!c->led_mask || c->torch_id >= DBT_CHANNELS || c->hard_ma < c->ires_ua / 1000u)
        return stop_at(t, -1, "config", -22);
    DbtConfig *k = &t->cfg;
    if (k->max_ma > k->hard_ma) k->max_ma = k->hard_ma;
    if (!k->auto_off_ms || k->auto_off_ms > DBT_AUTO_OFF_MS) k->auto_off_ms = DBT_AUTO_OFF_MS;
    if (!k->poll_ms) k->poll_ms = DBT_POLL_DEFAULT_MS;
    if (k->poll_ms < DBT_POLL_MIN_MS) k->poll_ms = DBT_POLL_MIN_MS;
    if (k->poll_ms > DBT_POLL_MAX_MS) k->poll_ms = DBT_POLL_MAX_MS;

    /* The ceilings, both of them, in codes: hard is what the gate will ever sign, max is what a full
     * level means. The vendor computes both the same way, at [V]:1355 and [V]:1375. */
    unsigned idx = 0, ires = 0, hard_code = 0, code_max = 0;
    dbt_current_law(k->hard_ma, k->ires_ua, k->hard_ma, &idx, &ires, &hard_code);
    dbt_current_law(k->max_ma, k->ires_ua, k->hard_ma, &idx, &ires, &code_max);
    t->hard_code = (int)hard_code;
    t->code_max = (int)code_max;

    int rc = dbt_spmi_discover(&t->spmi, io, k->ee);
    spmi_line(t, rc);
    if (rc) return stop_at(t, -2, "arbiter", rc);
    t->stage = 2;
    if (k->mode == DBT_MODE_DRY) {
        rc = identity_and_dump(t);
        if (rc) return stop_at(t, -3, "dump-read", rc);
        t->state = DBT_ST_DRY;
        t->stage = 5;
        char buf[LINE];
        DbtText x;
        begin(&x, buf);
        dbt_text_str(&x, "dry run done: nothing written");
        dbt_text_kd(&x, "hard_ma", (int64_t)k->hard_ma);
        dbt_text_kd(&x, "max_ma", (int64_t)k->max_ma);
        dbt_text_kd(&x, "code", (int64_t)code_max);
        dbt_text_kd(&x, "reads", (int64_t)t->spmi.reads);
        trace(t, buf);
        return 1;
    }
    t->stage = 3;
    rc = dbt_spmi_open(&t->spmi, hard_code);
    if (rc) return stop_at(t, -4, "open", rc);
    /* The light goes down before anything is read for evidence: the boot values of this block are
     * the vendor camera stack's, and it leaves the module and the torch channel enabled. Every write
     * below is a clear, so the interlock cannot block it, and the read-back is what says the light
     * is really out before this port calls itself ready. */
    t->stage = 4;
    uint8_t en = 0, mod = 0;
    rc = rd(t, DBT_R_EN_LED_CTRL, &en);
    if (!rc) rc = rd(t, DBT_R_MOD_CTRL, &mod);
    if (rc) return fault(t, "init-read", rc);
    if (en & DBT_LED_MASK) rc = wr(t, DBT_R_EN_LED_CTRL, DBT_LED_MASK, 0);
    if (!rc && (mod & DBT_M_MOD_CTRL)) rc = wr(t, DBT_R_MOD_CTRL, DBT_M_MOD_CTRL, DBT_V_MOD_DISABLE);
    if (rc) return fault(t, "init-write", rc);
    rc = rd(t, DBT_R_EN_LED_CTRL, &en);
    if (!rc) rc = rd(t, DBT_R_MOD_CTRL, &mod);
    if (rc) return fault(t, "init-verify-read", rc);
    if ((en & DBT_LED_MASK) || (mod & DBT_M_MOD_CTRL)) return fault(t, "init-verify", -5);
    t->stage = 5;
    rc = identity_and_dump(t);
    if (rc) return fault(t, "dump-read", rc);
    t->state = DBT_ST_OFF;

    char buf[LINE];
    DbtText x;
    begin(&x, buf);
    dbt_text_str(&x, "ready");
    dbt_text_kd(&x, "max_ma", (int64_t)k->max_ma);
    dbt_text_kd(&x, "hard_ma", (int64_t)k->hard_ma);
    dbt_text_kd(&x, "code", (int64_t)code_max);
    dbt_text_kd(&x, "hard_code", (int64_t)hard_code);
    dbt_text_kd(&x, "apid", t->spmi.apid);
    dbt_text_kd(&x, "off_s", (int64_t)(k->auto_off_ms / 1000u));
    dbt_text_kd(&x, "reads", (int64_t)t->spmi.reads);
    dbt_text_kd(&x, "writes", (int64_t)t->spmi.writes);
    trace(t, buf);
    return DBT_OK;
}

/* The vendor's thermal cap, applied the way its own torch-current branch does it: TGR_CURRENT alone
 * ([V]:1667-1681). It may only come down; raising it again would be a policy nobody wrote. */
static int apply_derate(DbtCore *t, unsigned lim_ma)
{
    unsigned idx = 0, ires = 0, code = 0;
    dbt_current_law(lim_ma, t->cfg.ires_ua, t->cfg.hard_ma, &idx, &ires, &code);
    if (code >= (unsigned)t->code) return DBT_OK;
    const int rc = wr(t, DBT_R_TGR_CURRENT, DBT_M_CURRENT, (uint8_t)code);
    if (rc) return fault(t, "derate-write", rc);
    uint8_t v = 0;
    const int rr = rd(t, DBT_R_TGR_CURRENT, &v);
    if (rr) return fault(t, "derate-read", rr);
    if ((unsigned)(v & DBT_M_CURRENT) != code) return fault(t, "derate-verify", -5);
    ++t->derates;
    t->ma = lim_ma;
    t->code = code;
    char buf[LINE];
    DbtText x;
    begin(&x, buf);
    dbt_text_str(&x, "derate");
    dbt_text_kd(&x, "otst", (int64_t)(t->sts[1] & DBT_M_OTST));
    dbt_text_kd(&x, "ma", (int64_t)lim_ma);
    dbt_text_kd(&x, "code", (int64_t)code);
    dbt_text_kd(&x, "derates", (int64_t)t->derates);
    trace(t, buf);
    return DBT_OK;
}

/* ---- on / off ---- */
static int torch_off(DbtCore *t)
{
    uint8_t en = 0, mod = 0, v = 0;
    /* The two reads that decide which bytes have to be cleared are named for what they are: a merge
     * under a masked write is reported as the write, but these are plain reads of the enable and the
     * module byte, and the operator needs to see off-read rather than a write that never ran. */
    int rc = rd(t, DBT_R_EN_LED_CTRL, &en);
    if (rc) return fault(t, "off-read", rc);
    rc = rd(t, DBT_R_MOD_CTRL, &mod);
    if (rc) return fault(t, "off-read", rc);
    if (en & DBT_LED_MASK) rc = wr(t, DBT_R_EN_LED_CTRL, DBT_LED_MASK, 0);
    if (!rc && (mod & DBT_M_MOD_CTRL)) rc = wr(t, DBT_R_MOD_CTRL, DBT_M_MOD_CTRL, DBT_V_MOD_DISABLE);
    /* [V]:1437-1445: the target current goes away and the disabled timer is written last. */
    if (!rc) rc = wr(t, DBT_R_TGR_CURRENT, DBT_M_CURRENT, 0);
    if (!rc) rc = wr(t, DBT_R_SAFETY_TMR, 0xFFu, (uint8_t)DBT_TORCH_TMR);
    if (rc) return fault(t, "off-write", rc);
    /* really off: a write the arbiter acknowledged is not the same thing as a light that is out. The
     * three read-backs are checked one after another and each failure stops the call - a read that
     * did not answer leaves the previous byte in `v`, and a stale zero is not a verified zero. */
    rc = rd(t, DBT_R_EN_LED_CTRL, &v);
    if (rc) return fault(t, "off-read", rc);
    if (v & DBT_LED_MASK) return fault(t, "off-verify", -5);
    rc = rd(t, DBT_R_MOD_CTRL, &v);
    if (rc) return fault(t, "off-read", rc);
    if (v & DBT_M_MOD_CTRL) return fault(t, "off-verify", -5);
    rc = rd(t, DBT_R_TGR_CURRENT, &v);
    if (rc) return fault(t, "off-read", rc);
    if (v & DBT_M_CURRENT) return fault(t, "off-verify", -5);
    t->state = DBT_ST_OFF;
    t->level = 0;
    t->ma = 0;
    t->code = 0;
    return DBT_OK;
}

static int torch_on(DbtCore *t, unsigned code, unsigned ires_idx)
{
    uint8_t en = 0, mod = 0, v = 0;
    int rc = rd(t, DBT_R_EN_LED_CTRL, &en);
    if (!rc) rc = rd(t, DBT_R_MOD_CTRL, &mod);
    if (rc) return fault(t, "on-read", rc);
    /* [V]:1662 IRES first, then [V]:1694-1708 strobe, current, timer, for channel 0 only. */
    if (!rc && (en & DBT_LED_MASK)) rc = wr(t, DBT_R_EN_LED_CTRL, DBT_LED_MASK, 0);
    if (!rc && (mod & DBT_M_MOD_CTRL)) rc = wr(t, DBT_R_MOD_CTRL, DBT_M_MOD_CTRL, DBT_V_MOD_DISABLE);
    if (!rc) rc = wr(t, DBT_R_IRES, DBT_M_CURRENT, (uint8_t)ires_idx);
    if (!rc) rc = wr(t, DBT_R_STROBE_CTRL, DBT_M_STROBE_SEL, (uint8_t)DBT_STROBE_CTRL_SW);
    if (!rc) rc = wr(t, DBT_R_TGR_CURRENT, DBT_M_CURRENT, (uint8_t)code);
    if (!rc) rc = wr(t, DBT_R_SAFETY_TMR, 0xFFu, (uint8_t)DBT_TORCH_TMR);
    if (rc) return fault(t, "on-write", rc);

    /* Read back before the channel is switched on: a write the arbiter acknowledged but the PMIC did
     * not take would otherwise light the LED at a current nobody asked for. */
    rc = rd(t, DBT_R_IRES, &v);
    if (!rc && (unsigned)(v & DBT_M_CURRENT) != ires_idx) return fault(t, "verify-ires", -5);
    if (!rc) {
        rc = rd(t, DBT_R_STROBE_CTRL, &v);
        if (!rc && (v & DBT_M_STROBE_SEL)) return fault(t, "verify-strobe", -5);
    }
    if (!rc) {
        rc = rd(t, DBT_R_TGR_CURRENT, &v);
        if (!rc && (unsigned)(v & DBT_M_CURRENT) != code) return fault(t, "verify-current", -5);
    }
    if (!rc) {
        rc = rd(t, DBT_R_SAFETY_TMR, &v);
        if (!rc && v != (uint8_t)DBT_TORCH_TMR) return fault(t, "verify-timer", -5);
    }
    if (rc) return fault(t, "verify-read", rc);

    /* qpnp_flash_led_module_enable ([V]:1589-1609), then the channel itself ([V]:1764). */
    rc = wr(t, DBT_R_MOD_CTRL, DBT_M_MOD_CTRL, DBT_V_MOD_ENABLE);
    if (!rc) rc = rd(t, DBT_R_MOD_CTRL, &v);
    if (rc) return fault(t, "module-write", rc);
    if (!(v & DBT_M_MOD_CTRL)) return fault(t, "module-verify", -5);
    rc = wr(t, DBT_R_EN_LED_CTRL, DBT_LED_MASK, DBT_V_ENABLE);
    if (rc) return fault(t, "enable-write", rc);
    /* and the same rule the off path already applies: an acknowledged write is not a light that is
     * lit, so the enable bit is read back before the call reports success. */
    rc = rd(t, DBT_R_EN_LED_CTRL, &v);
    if (rc) return fault(t, "enable-read", rc);
    if (!(v & DBT_LED_MASK)) return fault(t, "verify-enable", -5);

    t->state = DBT_ST_ON;
    t->code = code;
    t->ires_idx = ires_idx;
    rc = read_status(t, t->sts);
    if (rc) return fault(t, "status-read", rc);
    const char *f = fault_of(t);
    if (f) {
        lamp_fault(t, f);
        return -5;
    }
    const unsigned lim = thermal_limit_ma(t->sts[1]);
    if (lim && lim < t->ma) return apply_derate(t, lim);
    return DBT_OK;
}

static void event_line(DbtCore *t, const char *what, const char *why)
{
    if (!event_line_allowed(t)) return;
    char buf[LINE];
    DbtText x;
    begin(&x, buf);
    dbt_text_str(&x, what);
    dbt_text_kd(&x, "level", (int64_t)t->level);
    dbt_text_kd(&x, "ma", (int64_t)t->ma);
    dbt_text_kd(&x, "code", (int64_t)t->code);
    dbt_text_kd(&x, "ires", (int64_t)t->ires_idx);
    dbt_text_ks(&x, "by", why ? why : "-");
    const uint64_t n = now_us(t);
    if (t->state == DBT_ST_ON) {
        dbt_text_kd(&x, "off_in_ms", t->deadline_us > n ? (int64_t)((t->deadline_us - n) / 1000u) : 0);
    } else {
        dbt_text_kd(&x, "on_ms", (int64_t)((n - t->on_us) / 1000u));
        dbt_text_kd(&x, "ons", (int64_t)t->ons);
    }
    if (t->derates) dbt_text_kd(&x, "derates", (int64_t)t->derates);
    if (t->dropped) dbt_text_kd(&x, "dropped", (int64_t)t->dropped);
    trace(t, buf);
}

int dbt_core_set(DbtCore *t, unsigned level, uint32_t hold_ms, const char *why)
{
    if (!t || !t->ops) return -22;
    if (t->state == DBT_ST_FAULT) return -5;
    if (t->state != DBT_ST_OFF && t->state != DBT_ST_ON) return DBT_NODEV;
    if (level > DBT_LEVEL_MAX) return -22;
    if (!level) {
        if (t->state != DBT_ST_ON) return DBT_OK;
        const int rc = torch_off(t);
        if (rc) return rc;
        ++t->offs;
        event_line(t, "off", why);
        return DBT_OK;
    }

    const unsigned ma = dbt_level_ma(level, t->cfg.max_ma);
    unsigned idx = 0, ires = 0, code = 0;
    dbt_current_law(ma, t->cfg.ires_ua, t->cfg.hard_ma, &idx, &ires, &code);
    if (code > (unsigned)t->code_max) code = (unsigned)t->code_max;

    const int was_on = t->state == DBT_ST_ON;
    if (!was_on || code != t->code || idx != t->ires_idx) {
        t->ma = ma;
        const int rc = torch_on(t, code, idx);
        if (rc) return rc;
    }
    t->level = level;
    const uint64_t n = now_us(t);
    if (!was_on) {
        ++t->ons;
        t->on_us = n;
        uint64_t ms = t->cfg.auto_off_ms;
        if (hold_ms && hold_ms < ms) ms = hold_ms;
        t->deadline_us = n + ms * 1000u;
    } else if (hold_ms && n + (uint64_t)hold_ms * 1000u < t->deadline_us) {
        t->deadline_us = n + (uint64_t)hold_ms * 1000u;
    }
    event_line(t, was_on ? "level" : "on", why);
    return DBT_OK;
}

uint32_t dbt_core_poll(DbtCore *t)
{
    if (!t || !t->ops || t->state != DBT_ST_ON) return 0;
    int rc = read_status(t, t->sts);
    if (rc) {
        fault(t, "status-read", rc);
        return 0;
    }
    const char *f = fault_of(t);
    if (f) {
        lamp_fault(t, f);
        return 0;
    }
    const unsigned lim = thermal_limit_ma(t->sts[1]);
    if (lim && lim < t->ma) apply_derate(t, lim);
    const uint64_t n = now_us(t);
    if (n >= t->deadline_us) {
        if (torch_off(t) == DBT_OK) {
            ++t->offs;
            event_line(t, "auto-off", "deadline");
        }
        return 0;
    }
    return t->cfg.poll_ms;
}

void dbt_core_export(const DbtCore *t, int32_t out[16])
{
    out[0] = (int32_t)t->state;
    out[1] = (int32_t)t->stage;
    out[2] = (int32_t)t->level;
    out[3] = (int32_t)t->ma;
    out[4] = (int32_t)t->code;
    out[5] = (int32_t)t->ires_idx;
    out[6] = (int32_t)t->faults;
    out[7] = (int32_t)t->ons;
    out[8] = (int32_t)t->offs;
    out[9] = (int32_t)t->derates;
    out[10] = (int32_t)t->spmi.reads;
    out[11] = (int32_t)t->spmi.writes;
    out[12] = (int32_t)t->spmi.refused;
    out[13] = (int32_t)t->spmi.errors;
    out[14] = (int32_t)t->spmi.apid;
    out[15] = (int32_t)t->spmi.last_status;
}
