/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The host model the torch sources are tested against: the SM8250 v5 SPMI arbiter as DbyBattery.cpp
 * proved it on this tablet, plus a PM8150L flash-v2 block as leds-qpnp-flash-v2.c describes it. The
 * model is deliberately dumb about policy - it only answers to the command words the arbiter really
 * takes, it applies the bytes it really takes, and its LED says what the hardware would have done.
 * Everything it knows about the hardware comes from DbyTorchRegs.h's [V] and [D] citations, so a
 * disagreement between the port and the model is a disagreement about the vendor's driver. */
#ifndef DBY_TORCH_TEST_MODEL_H
#define DBY_TORCH_TEST_MODEL_H

#include <string.h>

#include "../DbyTorchCore.h"
#include "check.h"

#define M_APIDS     64u               /* modelled map cells; the port reads 512, the rest answer 0 */
#define M_AFLASH    12u               /* the flash block's channel, owned by this EE */
#define M_AFOREIGN   5u               /* a second mapping of the same PPID, owned by EE 2 */
#define M_PPID      ((uint32_t)DBT_PPID)
#define M_LOG     8000u
#define M_LINES    400u
#define M_LINE     200u
#define M_BLOCK    256u
#define M_FORM_MASK   7u              /* all three command shapes of one ladder, as a bit mask */

/* What one command-word shape does when it is sent: answer, answer DONE with an error bit, or never
 * answer at all. Three and not two, because spmi-pmic-arb.c's wait_for_done() keeps those apart and
 * the port's ladder branches on exactly that difference. */
enum { M_ANS = 0, M_ERR = 1, M_SILENT = 2 };

typedef struct {
    uint8_t reg, value;
    uint64_t t;
} MWrite;

typedef struct {
    /* ---- the v5 arbiter ---- */
    uint32_t version;                 /* PMIC_ARB_VERSION at core+0x00 */
    uint32_t map[M_APIDS];            /* core + 0x900 + 4n: PPID at 19:8, irq owner at bit 24 */
    uint32_t owner[M_APIDS];          /* cnfg + 0x700 + 4n: the write EE at 2:0 */
    uint32_t obs_sts[M_APIDS], obs_rdata[M_APIDS];
    uint32_t ch_sts[M_APIDS], ch_w0[M_APIDS];
    int mapped;                       /* the apid whose RW page chan_map handed back, -1 none */

    /* ---- the flash-v2 block, 256 bytes ---- */
    uint8_t reg[M_BLOCK];

    /* ---- the clock, the LED and the record ---- */
    uint64_t now_us, tmr_off_us;
    int tmr_armed, lit;
    unsigned ma;                      /* what the hardware would be drawing right now, in mA */
    unsigned delays, core_reads, cnfg_reads, obs_cmds, ch_cmds, map_calls;
    unsigned bad_obs, bad_ch;         /* shapes a real arbiter would not take: must stay 0 */
    MWrite log[M_LOG];
    unsigned log_n;
    char lines[M_LINES][M_LINE];
    unsigned line_n;

    /* ---- injection ---- */
    int rpol[3], wpol[3];             /* per command-word shape, read and write side */
    int fail_writes_after, fail_reads_after;   /* -1 never, 0 the next one, n after n more */
    /* One transaction of a ladder failing is not a failure: DbyTorchSpmi.c walks its three shapes and
     * normally recovers. These two knobs take the whole ladder down instead, which is the only way to
     * make the core's failure branches reachable. They are indexed by transaction number, so "the k-th
     * write of the run" means the k-th command word the arbiter was given, counting from zero. */
    int fail_w_txn, fail_r_txn;       /* -1 never */
    uint32_t fail_status;             /* the status word a failing transaction reports */
    int never_done, chan_map_fail, hide_flash_map;
    uint8_t stuck_reg;
    int stuck_on;                     /* the arbiter says DONE, the PMIC keeps the old byte */
    uint8_t fault_s1, fault_s2, fault_s3;      /* ORed into the three status bytes */
    /* The clock of this model only moves when the test moves it, so "the byte is already hot when the
     * light comes on" cannot be expressed as a deadline one microsecond ahead - nothing would ever
     * reach it. fault_armed is the switch and fault_at_us the moment from which it reports; with the
     * switch on and the moment at now, the very next status read carries the bits. */
    int fault_armed;
    uint64_t fault_at_us;
    /* A dead ladder in progress. One operation of the port's ladder is the same register on every
     * command shape it has - at most the remembered one plus the three of the walk - so the window
     * closes exactly when all three shapes have been seen on that register. That is what keeps the
     * injection from bleeding into the next operation, which would be a model artefact and not a
     * property of the port. */
    uint8_t wop_reg, rop_reg;
    uint8_t wop_mask, rop_mask;
    int wop_dead, rop_dead;
} Model;

/* ---- the module's own semantics ([V] qpnp_flash_led_switch_set, the enable bits it writes) ---- */
/* The light is lit when the module is enabled, this board's one channel (led_mask 0x1) is enabled and
 * the channel is *not* left on the hardware trigger. The current is the programmed code times the
 * resolution the IRES byte selects for this channel - two bits per channel in v2 ([V]:1662 writes
 * ires_idx << (id * 2)), not one as the older v1 block uses. */
static inline void m_settle(Model *m)
{
    const unsigned idx = (unsigned)((m->reg[DBT_R_IRES] >> (DBT_TORCH_ID * 2u)) & 3u);
    const unsigned ires_ua = DBT_IRES_MIN_UA + (DBT_IRES_BASE - idx) * DBT_IRES_DIVISOR;
    m->lit = (m->reg[DBT_R_MOD_CTRL] & DBT_M_MOD_CTRL) != 0 &&
             (m->reg[DBT_R_EN_LED_CTRL] & DBT_LED_MASK) != 0 &&
             (m->reg[DBT_R_STROBE_CTRL] & DBT_M_STROBE_SEL) == 0;
    m->ma = m->lit ? ((unsigned)(m->reg[DBT_R_TGR_CURRENT] & DBT_M_CURRENT) + 1u) * ires_ua / 1000u : 0u;
    if (m->lit && (m->reg[DBT_R_SAFETY_TMR] & 0x80u)) {
        m->tmr_armed = 1;
        m->tmr_off_us = m->now_us + (uint64_t)(m->reg[DBT_R_SAFETY_TMR] & 0x7Fu) * 10000ull;
    } else if (!m->lit) {
        m->tmr_armed = 0;
        m->tmr_off_us = 0;
    }
}

static inline void m_advance(Model *m, uint64_t us)
{
    m->now_us += us;
    /* the hardware's own safety timer, which is why the port writes the disabled 0x13 */
    if (m->tmr_armed && m->now_us >= m->tmr_off_us) {
        m->reg[DBT_R_EN_LED_CTRL] = (uint8_t)(m->reg[DBT_R_EN_LED_CTRL] & ~DBT_LED_MASK);
        m_settle(m);
    }
}

static inline uint8_t m_read(Model *m, uint8_t reg)
{
    if (m->fault_armed && m->now_us >= m->fault_at_us) {
        if (reg == DBT_R_STATUS1) return (uint8_t)(m->reg[reg] | m->fault_s1);
        if (reg == DBT_R_STATUS2) return (uint8_t)(m->reg[reg] | m->fault_s2);
        if (reg == DBT_R_INT_RT_STS) return (uint8_t)(m->reg[reg] | m->fault_s3);
    }
    return m->reg[reg];
}

/* Say what the module reports from this moment on. The test's clock only moves when the test moves
 * it, so the bits have to be armed at the time they are meant to already be true. */
static inline void m_fault_now(Model *m, uint8_t s1, uint8_t s2, uint8_t s3)
{
    m->fault_s1 = s1;
    m->fault_s2 = s2;
    m->fault_s3 = s3;
    m->fault_at_us = m->now_us;
    m->fault_armed = 1;
}

/* ---- the command word ---- */
/* The six shapes the port's two ladders can send, and nothing else. The extended spellings take the
 * block from the APID and carry the SID as zero; the third of each ladder names the USID at 23:20
 * (spmi-pmic-arb.c's fmt_cmd_v1 shape, kept as the port's last resort). The length term is bits 2:0
 * and one byte is len-1 == 0. */
static inline int m_form_read(uint32_t cmd, unsigned *idx)
{
    const uint32_t op = cmd >> 27, sid = (cmd >> 20) & 0xFu;
    if (op == 1u) { if (sid) return -1; *idx = 0u; return 1; }
    if (op == 15u) { if (sid) return -1; *idx = 1u; return 1; }
    if (op == 13u) { if (sid != DBT_USID) return -1; *idx = 2u; return 1; }
    return -1;
}
static inline int m_form_write(uint32_t cmd, unsigned *idx)
{
    const uint32_t op = cmd >> 27, sid = (cmd >> 20) & 0xFu;
    if (op == 0u) { if (sid) return -1; *idx = 0u; return 1; }
    if (op == 14u) { if (sid) return -1; *idx = 1u; return 1; }
    if (op == 2u) { if (sid != DBT_USID) return -1; *idx = 2u; return 1; }
    return -1;
}

/* ---- DbtSpmiIo ---- */
static inline uint32_t m_core(void *ctx, uint32_t at)
{
    Model *m = ctx;
    m->core_reads++;
    if (at == 0) return m->version;
    CHECK(at >= DBT_ARB_APID_MAP && at < DBT_ARB_CORE_SZ && !(at & 3u));
    const unsigned n = (at - DBT_ARB_APID_MAP) / 4u;
    if (n >= M_APIDS) return 0;                       /* the port reads 512 cells; only 64 exist */
    if (m->hide_flash_map && (((m->map[n] >> 8) & 0xFFFu) == M_PPID)) return 0;
    return m->map[n];
}
static inline uint32_t m_cnfg(void *ctx, uint32_t at)
{
    Model *m = ctx;
    m->cnfg_reads++;
    CHECK(at >= DBT_ARB_OWNERSHIP && at < DBT_ARB_CNFG_SZ && !(at & 3u));
    const unsigned n = (at - DBT_ARB_OWNERSHIP) / 4u;
    if (n >= M_APIDS) return 0xFFF0u;
    return m->owner[n] | 0xFFF0u;                     /* only bits 2:0 are the write EE */
}
static inline uint32_t m_obs(void *ctx, uint32_t at)
{
    Model *m = ctx;
    const unsigned apid = at / DBT_ARB_OBS_APID_STRIDE, off = at % DBT_ARB_OBS_APID_STRIDE;
    CHECK(apid < M_APIDS);
    if (off == DBT_ARB_STATUS) return m->obs_sts[apid];
    if (off == DBT_ARB_RDATA0) return m->obs_rdata[apid];
    return 0;
}
static inline void m_obs_put(void *ctx, uint32_t at, uint32_t v)
{
    Model *m = ctx;
    const unsigned apid = at / DBT_ARB_OBS_APID_STRIDE;
    if (at % DBT_ARB_OBS_APID_STRIDE != DBT_ARB_CMD || apid >= M_APIDS) { m->bad_obs++; return; }
    m->obs_cmds++;
    unsigned idx = 0;
    if (m_form_read(v, &idx) < 0) { m->bad_obs++; m->obs_sts[apid] = 3u; return; }
    if (v & 7u) m->bad_obs++;                          /* more than one byte was asked for */
    m->obs_rdata[apid] = 0;
    if (m->never_done || m->rpol[idx] == M_SILENT) { m->obs_sts[apid] = 0; return; }
    if (m->rpol[idx] == M_ERR) { m->obs_sts[apid] = m->fail_status; return; }
    if (m->fail_reads_after == 0) { m->fail_reads_after = -1; m->obs_sts[apid] = m->fail_status; return; }
    if (m->fail_reads_after > 0) m->fail_reads_after--;
    const uint8_t reg = (uint8_t)((v >> 4) & 0xFFu);
    if (m->rop_reg != reg) { m->rop_reg = reg; m->rop_dead = 0; m->rop_mask = 0; }
    if (m->fail_r_txn >= 0 && (int)(m->obs_cmds - 1u) == m->fail_r_txn) {
        m->rop_dead = 1;
        m->rop_mask = 0;
    }
    if (m->rop_dead) {
        if (m->rop_mask == M_FORM_MASK) m->rop_dead = 0;        /* that ladder has been walked */
        else {
            m->rop_mask = (uint8_t)(m->rop_mask | (1u << idx));
            m->obs_sts[apid] = m->fail_status;
            return;
        }
    }
    uint32_t data = 0;
    if (((m->map[apid] >> 8) & 0xFFFu) == M_PPID)      /* a read of another channel answers zero */
        data = m_read(m, reg);
    m->obs_rdata[apid] = data;
    m->obs_sts[apid] = 1u;
}
static inline int m_chan_map(void *ctx, unsigned apid)
{
    Model *m = ctx;
    m->map_calls++;
    CHECK(apid < M_APIDS);
    if (m->chan_map_fail) return -12;                 /* what an IOMemoryMap failure would return */
    if (((m->map[apid] >> 8) & 0xFFFu) != M_PPID) return -2;
    m->mapped = (int)apid;
    return 0;
}
static inline uint32_t m_chan(void *ctx, uint32_t at)
{
    Model *m = ctx;
    CHECK(m->mapped >= 0 && at < DBT_ARB_CHNL_SZ);
    if (at == DBT_ARB_STATUS) return m->ch_sts[m->mapped];
    if (at == DBT_ARB_WDATA0) return m->ch_w0[m->mapped];
    return 0;
}
static inline void m_chan_put(void *ctx, uint32_t at, uint32_t v)
{
    Model *m = ctx;
    CHECK(m->mapped >= 0 && at < DBT_ARB_CHNL_SZ);
    const unsigned apid = (unsigned)m->mapped;
    if (at == DBT_ARB_WDATA0) { m->ch_w0[apid] = v; return; }
    if (at != DBT_ARB_CMD) { m->bad_ch++; return; }
    m->ch_cmds++;
    unsigned idx = 0;
    if (m_form_write(v, &idx) < 0) { m->bad_ch++; m->ch_sts[apid] = 3u; return; }
    if (v & 7u) m->bad_ch++;                           /* more than one byte was sent */
    const uint8_t reg = (uint8_t)((v >> 4) & 0xFFu);
    const uint8_t value = (uint8_t)m->ch_w0[apid];
    if (m->never_done || m->wpol[idx] == M_SILENT) { m->ch_sts[apid] = 0; return; }
    if (m->wpol[idx] == M_ERR) { m->ch_sts[apid] = m->fail_status; return; }
    if (m->fail_writes_after == 0) {
        m->fail_writes_after = -1;
        m->ch_sts[apid] = m->fail_status;
        return;
    }
    if (m->fail_writes_after > 0) m->fail_writes_after--;
    if (m->wop_reg != reg) { m->wop_reg = reg; m->wop_dead = 0; m->wop_mask = 0; }
    if (m->fail_w_txn >= 0 && (int)(m->ch_cmds - 1u) == m->fail_w_txn) {
        m->wop_dead = 1;
        m->wop_mask = 0;
    }
    if (m->wop_dead) {
        if (m->wop_mask == M_FORM_MASK) m->wop_dead = 0;        /* that ladder has been walked */
        else {
            m->wop_mask = (uint8_t)(m->wop_mask | (1u << idx));
            m->ch_sts[apid] = m->fail_status;
            return;
        }
    }
    if ((m->owner[apid] & 7u) != DBT_EE) { m->ch_sts[apid] = 5u; return; }   /* done | denied */
    m->ch_sts[apid] = 1u;
    if (m->stuck_on && m->stuck_reg == reg) return;    /* acknowledged, and the byte did not land */
    CHECK(m->log_n < M_LOG);
    m->reg[reg] = value;
    m->log[m->log_n].reg = reg;
    m->log[m->log_n].value = value;
    m->log[m->log_n].t = m->now_us;
    m->log_n++;
    m_settle(m);
}
static inline void m_delay(void *ctx, unsigned us) {Model *m = ctx; m->delays++; m_advance(m, us);}

/* ---- DbtOps ---- */
static inline uint64_t m_now(void *ctx) {return ((Model *)ctx)->now_us;}
static inline void m_trace(void *ctx, const char *line)
{
    Model *m = ctx;
    CHECK(strlen(line) < M_LINE);
    if (m->line_n < M_LINES) {
        strncpy(m->lines[m->line_n], line, M_LINE - 1u);
        m->line_n++;
    }
}
static inline int m_logged(const Model *m, const char *needle)
{
    for (unsigned i = 0; i < m->line_n; ++i) if (strstr(m->lines[i], needle)) return 1;
    return 0;
}
static inline unsigned m_seq(const Model *m, unsigned from, char *out, size_t size)
{
    size_t n = 0;
    for (unsigned i = from; i < m->log_n && n + 8u < size; ++i)
        n += (size_t)snprintf(out + n, size - n, "%s%02x=%02x", i == from ? "" : " ",
                              m->log[i].reg, m->log[i].value);
    out[n] = 0;
    return m->log_n - from;
}

static const DbtSpmiIo kModelIo = {0, m_core, m_cnfg, m_obs, m_obs_put, m_chan_map, m_chan,
                                   m_chan_put, m_delay};

/* PM8150L's flash block as this board's sources describe it, at boot values chosen to be off the
 * targets on purpose: the module is enabled, the torch channel is enabled, a flash current of 0x7f
 * and the 5 mA resolution are left in the registers, and the channel is still on the hardware
 * trigger. Taking over from that state is the case that must be right. */
static inline void m_init(Model *m)
{
    memset(m, 0, sizeof *m);
    m->version = 0x50060000u;                 /* v5: the range spmi-pmic-arb.c accepts for SM8250 */
    m->mapped = -1;
    m->fail_writes_after = -1;
    m->fail_reads_after = -1;
    m->fail_w_txn = -1;
    m->fail_r_txn = -1;
    m->fail_status = 0x3u;
    for (unsigned i = 0; i < 3u; ++i) { m->rpol[i] = M_ANS; m->wpol[i] = M_ANS; }
    for (unsigned n = 0; n < M_APIDS; ++n) {   /* other PMICs' blocks, several owned by other EEs */
        const uint32_t ppid = (((n % 6u) + 1u) << 8) | (0x10u + (n % 0x20u));
        m->map[n] = (ppid << 8) | (n % 7u == 0 ? 1u << 24 : 0);
        m->owner[n] = n % 3u;
    }
    m->map[M_AFOREIGN] = (uint32_t)M_PPID << 8;
    m->owner[M_AFOREIGN] = 2u;
    m->map[M_AFLASH] = ((uint32_t)M_PPID << 8) | (1u << 24);
    m->owner[M_AFLASH] = 0u;
    uint8_t *r = m->reg;
    r[DBT_R_TYPE] = 0x06u;                    /* evidence only: [V] validates no TYPE byte at all */
    r[DBT_R_SUBTYPE] = 0x00u;
    r[DBT_R_IRES] = 0x03u;                    /* the 5 mA step the camera stack left */
    r[DBT_R_TGR_CURRENT] = 0x7Fu;             /* and a target current left at the ceiling */
    r[DBT_R_SAFETY_TMR] = DBT_V_TMR_DISABLE;
    r[DBT_R_MOD_CTRL] = DBT_V_MOD_ENABLE;     /* the module is on when this port takes over */
    r[DBT_R_EN_LED_CTRL] = DBT_V_ENABLE;      /* and the torch channel is enabled */
    r[DBT_R_STROBE_CTRL] = DBT_M_STROBE_SEL;  /* still on the hardware trigger */
    r[DBT_R_HDRM_PRGM] = 0x14u;
    r[DBT_R_THRSH1] = 1u;
    r[DBT_R_THRSH2] = 2u;
    r[DBT_R_THRSH3] = 3u;
    m_settle(m);
}

static inline DbtConfig m_config(unsigned mode, unsigned max_ma)
{
    DbtConfig c;
    CHECK(dbt_config_make(&c, mode, DBT_EE, DBT_TORCH_MAX_MA, max_ma, 2) == 1);
    return c;
}

static inline DbtOps m_ops(Model *m)
{
    DbtOps o = {m, m_now, m_trace};
    return o;
}

static inline DbtSpmiIo m_io(Model *m)
{
    DbtSpmiIo io = kModelIo;
    io.ctx = m;
    return io;
}

/* Runs the core's poll loop on the simulated clock until it reports nothing to do (or the limit). */
static inline void m_run(Model *m, DbtCore *c, uint64_t limit_us)
{
    const uint64_t end = m->now_us + limit_us;
    while (m->now_us < end) {
        const uint32_t ms = dbt_core_poll(c);
        if (!ms) break;
        m_advance(m, (uint64_t)ms * 1000u);
    }
}

/* index of the first logged write of reg at or after `from`, -1 none */
static inline int m_find(const Model *m, unsigned from, uint8_t reg)
{
    for (unsigned i = from; i < m->log_n; ++i) if (m->log[i].reg == reg) return (int)i;
    return -1;
}

#endif
