/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host tests of DbyTorchSpmi.c against a model of the SM8250 v5 arbiter and the PM8150L flash-v2
 * block: discovery over the whole APID table, the owned-mapping preference, the command-word shapes
 * the arbiter really takes, the ladder's memory and its walking, the write whitelist over every one
 * of the 256 bytes of the block, the interlock that keeps the channel off until the gate itself has
 * written the four bytes that decide the current, ownership that is not this EE's, a silent engine,
 * a failing transaction, and the one emergency stop. */
#include "model.h"

static Model m;
static DbtSpmi s;

/* every address the gate lets a given (mask, value) through, as "43 47 ..." */
static void gate_list(const DbtSpmi *g, uint8_t mask, uint8_t value, char *out, size_t size)
{
    size_t n = 0;
    for (unsigned a = 0; a < 256u; ++a) {
        if (!dbt_spmi_allowed(g, (uint8_t)a, mask, value)) continue;
        n += (size_t)snprintf(out + n, size - n, "%s%02x", n ? " " : "", a);
    }
    out[n] = 0;
}
#define GATE(mask, value, want) do {                                              \
        char g[512];                                                              \
        gate_list(&s, (uint8_t)(mask), (uint8_t)(value), g, sizeof g);             \
        if (strcmp(g, want) != 0) {                                               \
            fprintf(stderr, "%s:%d: gate mask=0x%02x value=0x%02x\n  got:  [%s]\n  want:[%s]\n", \
                    __FILE__, __LINE__, (unsigned)(mask), (unsigned)(value), g, want);           \
            exit(1);                                                              \
        }                                                                         \
        ++checks;                                                                 \
    } while (0)

/* Nothing may ever hand the arbiter a shape it would not take. */
#define CLEAN() do { CHECK(m.bad_obs == 0 && m.bad_ch == 0); } while (0)

static void all_write_shapes_fail(void) { m.wpol[0] = m.wpol[1] = m.wpol[2] = M_ERR; }

/* The four answers have to be four numbers. The whole point of walking the ladder shape by shape, and
 * of printing rc= and stop= on the FAULT line, is that the operator can tell a byte the gate would not
 * sign from an arbiter that stopped answering from one that answered with an error bit - and this
 * board's other SPMI consumer, DbyBattery.cpp, reads these same errnos out of the same calls. */
static void codes(void)
{
    CHECK(DBT_OK == 0);
    CHECK(DBT_PERM == -1 && DBT_ERROR == -5 && DBT_NODEV == -19 && DBT_TIMEOUT == -110);
    CHECK(DBT_PERM != DBT_ERROR && DBT_PERM != DBT_TIMEOUT && DBT_PERM != DBT_NODEV);
    CHECK(DBT_ERROR != DBT_TIMEOUT && DBT_ERROR != DBT_NODEV && DBT_TIMEOUT != DBT_NODEV);
}

static void discovered(void)
{
    m_init(&m);
    const DbtSpmiIo io = m_io(&m);
    memset(&s, 0, sizeof s);
    CHECK(dbt_spmi_discover(&s, &io, DBT_EE) == DBT_OK);
    CHECK(s.apid == (int)M_AFLASH && s.owner == 0 && s.irq_owner == 1);
    CHECK(s.rform == -2 && s.wform == -2);   /* nothing has answered yet; zero would be a claim */
    CHECK(s.ppid_hits == 2u);                /* this PPID is mapped twice among the modelled cells */
    CHECK(s.slots == M_APIDS);               /* every other cell is some other peripheral */
    /* the whole declared table is scanned, and the ownership table is consulted only for a cell that
     * actually claims this PPID */
    CHECK(m.core_reads == 1u + 512u && m.cnfg_reads == 2u);
    CHECK(s.reads == 0u && s.writes == 0u && m.obs_cmds == 0u && m.ch_cmds == 0u && m.map_calls == 0u);
    CLEAN();
}

static void wrong_bus(void)
{
    /* a v7 arbiter (the board the torch parent was written for) and a v4 one are both not this
     * tablet: the port must stop before it reads a single register, because the geometry it would
     * then use is the wrong geometry. */
    static const uint32_t bad[] = {0x70060000u, 0x4FFFFFFFu, 0x00000000u, 0xFFFFFFFFu};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        m_init(&m);
        m.version = bad[i];
        const DbtSpmiIo io = m_io(&m);
        DbtSpmi g;
        memset(&g, 0, sizeof g);
        CHECK(dbt_spmi_discover(&g, &io, DBT_EE) == DBT_NODEV);
        CHECK(g.apid < 0 && g.version == bad[i]);
        CHECK(m.core_reads == 1u && m.cnfg_reads == 0u && m.obs_cmds == 0u);
    }
    /* a table that names no such peripheral at all */
    m_init(&m);
    m.hide_flash_map = 1;
    const DbtSpmiIo io = m_io(&m);
    DbtSpmi g;
    memset(&g, 0, sizeof g);
    CHECK(dbt_spmi_discover(&g, &io, DBT_EE) == DBT_NODEV);
    /* both of the model's two cells that named this PPID answer zero now, so the port counted 62
     * populated cells and not one of them is the flash block */
    CHECK(g.apid < 0 && g.ppid_hits == 0u && g.slots == M_APIDS - 2u);
    CLEAN();
}

static void preference(void)
{
    /* pmic_arb_read_apid_map_v5 prefers a mapping for this EE. The first hit here (M_AFOREIGN) is
     * another EE's, the second (M_AFLASH) is this EE's, and the second is the one the port must keep. */
    m_init(&m);
    DbtSpmiIo io = m_io(&m);
    memset(&s, 0, sizeof s);
    CHECK(dbt_spmi_discover(&s, &io, DBT_EE) == DBT_OK && s.apid == (int)M_AFLASH);
    CHECK(dbt_spmi_open(&s, 23u) == DBT_OK && m.map_calls == 1u);
    /* reversed: the first hit is now this EE's and the second is not. The owner reported has to be
     * the chosen cell's, not the last cell the scan happened to look at. */
    m_init(&m);
    m.owner[M_AFLASH] = 2u;
    m.owner[M_AFOREIGN] = 0u;
    io = m_io(&m);
    memset(&s, 0, sizeof s);
    CHECK(dbt_spmi_discover(&s, &io, DBT_EE) == DBT_OK);
    CHECK(s.apid == (int)M_AFOREIGN && s.owner == 0);
    /* neither is ours: the apid is the first hit, the ownership table says why, and the RW page is
     * never asked for - spmi-pmic-arb.c's -EPERM check moved in front of the map */
    m_init(&m);
    m.owner[M_AFLASH] = 1u;
    m.owner[M_AFOREIGN] = 2u;
    io = m_io(&m);
    memset(&s, 0, sizeof s);
    CHECK(dbt_spmi_discover(&s, &io, DBT_EE) == DBT_OK);
    CHECK(s.apid == (int)M_AFOREIGN && s.owner == 2);
    CHECK(dbt_spmi_open(&s, 23u) == DBT_PERM);
    CHECK(s.denied == 1u && s.rw_mapped == 0 && m.map_calls == 0u);
    /* a channel the glue cannot map is not opened either, and says so */
    m_init(&m);
    m.chan_map_fail = 1;
    io = m_io(&m);
    memset(&s, 0, sizeof s);
    CHECK(dbt_spmi_discover(&s, &io, DBT_EE) == DBT_OK);
    CHECK(dbt_spmi_open(&s, 23u) == DBT_NODEV);
    CHECK(s.rw_mapped == 0 && m.map_calls == 1u && m.ch_cmds == 0u);
    CLEAN();
}

static void reads_first(void)
{
    m_init(&m);
    DbtSpmiIo io = m_io(&m);
    memset(&s, 0, sizeof s);
    CHECK(dbt_spmi_discover(&s, &io, DBT_EE) == DBT_OK);
    /* reading needs no RW page: a dry run is exactly this, and this much is enough to see the module */
    uint8_t v = 0;
    CHECK(dbt_spmi_read(&s, DBT_R_IRES, &v) == DBT_OK && v == 0x03u);
    CHECK(dbt_spmi_read(&s, DBT_R_TGR_CURRENT, &v) == DBT_OK && v == 0x7Fu);
    CHECK(dbt_spmi_read(&s, DBT_R_MOD_CTRL, &v) == DBT_OK && v == 0x80u);
    CHECK(s.reads == 3u && s.rform == 0 && s.wform == -2);
    /* one transaction per read once a shape is remembered, and not one poll: the engine answered */
    CHECK(m.obs_cmds == 3u && m.delays == 0u);
    /* and a write is refused outright before the page is mapped */
    CHECK(dbt_spmi_write(&s, DBT_R_IRES, DBT_M_CURRENT, 0u) == DBT_NODEV);
    CHECK(m.ch_cmds == 0u && m.log_n == 0u && s.writes == 0u);
    CHECK(dbt_spmi_open(&s, 23u) == DBT_OK && s.rw_mapped == 1 && m.map_calls == 1u);
    CHECK(m.ch_cmds == 0u);                 /* opening did not probe, and probing could not lie */
    CLEAN();
}

static void ladder(void)
{
    m_init(&m);
    DbtSpmiIo io = m_io(&m);
    memset(&s, 0, sizeof s);
    CHECK(dbt_spmi_discover(&s, &io, DBT_EE) == DBT_OK);
    /* the second spelling is the one that answers: the port must find it, then keep using it */
    m.rpol[0] = M_ERR;
    uint8_t v = 0;
    CHECK(dbt_spmi_read(&s, DBT_R_IRES, &v) == DBT_OK && v == 0x03u);
    CHECK(s.rform == 1 && s.errors == 1u && s.retries == 1u && m.obs_cmds == 2u);
    /* the third as well - the one that carries the SID, whose value the model checks */
    m.rpol[1] = M_ERR;
    CHECK(dbt_spmi_read(&s, DBT_R_IRES, &v) == DBT_OK);
    CHECK(s.rform == 2 && m.obs_cmds == 6u);   /* the remembered shape, then the whole ladder again */
    CHECK(s.errors == 4u && s.retries == 4u);
    CLEAN();
    /* a shape that stops answering at all ends the cycle instead of costing every later read a
     * thousand polls: this service shares its work loop with everything else */
    m.rpol[2] = M_SILENT;
    const unsigned cmds = m.obs_cmds;
    CHECK(dbt_spmi_read(&s, DBT_R_IRES, &v) == DBT_TIMEOUT);
    CHECK(s.silent == 1 && m.obs_cmds == cmds + 1u && m.delays == 1000u);
    const unsigned after = m.obs_cmds, delayed = m.delays;
    CHECK(dbt_spmi_read(&s, DBT_R_IRES, &v) == DBT_TIMEOUT);
    CHECK(dbt_spmi_read(&s, DBT_R_STATUS1, &v) == DBT_TIMEOUT);
    CHECK(m.obs_cmds == after && m.delays == delayed);      /* no polling restarted */
    /* the write side is stopped by the same evidence, before it sends anything */
    CHECK(dbt_spmi_open(&s, 23u) == DBT_OK);
    CHECK(dbt_spmi_write(&s, DBT_R_MOD_CTRL, DBT_M_MOD_CTRL, 0u) == DBT_TIMEOUT);
    CHECK(m.ch_cmds == 0u && m.log_n == 0u);
    /* and the write ladder settles on its own, without disturbing the read side */
    m_init(&m);
    io = m_io(&m);
    memset(&s, 0, sizeof s);
    CHECK(dbt_spmi_discover(&s, &io, DBT_EE) == DBT_OK && dbt_spmi_open(&s, 23u) == DBT_OK);
    CHECK(s.rform == -2 && s.wform == -2);
    m.wpol[0] = M_ERR;
    CHECK(dbt_spmi_write(&s, DBT_R_MOD_CTRL, DBT_M_MOD_CTRL, 0u) == DBT_OK);
    /* the write ladder settled on the second spelling; the read side settled too, on the merge read
     * that masked write has to make, and the two have nothing else to do with one another */
    CHECK(s.wform == 1 && s.rform == 0);
    CHECK(m.ch_cmds == 2u && m.obs_cmds == 1u && m.log_n == 1u);   /* one failed shape, one that landed */
    CHECK(m.reg[DBT_R_MOD_CTRL] == 0x00u);
    CLEAN();
}

static void whitelist(void)
{
    m_init(&m);
    DbtSpmiIo io = m_io(&m);
    memset(&s, 0, sizeof s);
    CHECK(dbt_spmi_discover(&s, &io, DBT_EE) == DBT_OK);
    CHECK(dbt_spmi_open(&s, 23u) == DBT_OK);        /* 23: the code of this board's 300 mA torch */
    CHECK(s.hard_code == 23u);
    /* a whole-byte write of zero is what a driver trying to "clear" the block would send: nothing on
     * this PMIC takes it, because every field the vendor writes has a mask of its own */
    GATE(0xFFu, 0x00u, "");
    /* the seven bytes the vendor's torch path touches, each only under its own mask */
    GATE(0x7Fu, 0x00u, "43 47");
    GATE(0x04u, 0x00u, "49");
    GATE(0x80u, 0x00u, "46");
    GATE(0x01u, 0x00u, "4c");
    GATE(0xFFu, 0x13u, "40");
    /* a wider led-mask is not this board's switch, a wider module mask is not the vendor's write, and
     * the status, strobe-config, HDRM and thermal bytes are not written by a torch at all */
    GATE(0x03u, 0x01u, "");
    GATE(0x80u, 0x81u, "");
    GATE(0x00u, 0x00u, "");
    GATE(0xFFu, 0x93u, "");                 /* an armed safety timer: never from software */
    GATE(0x04u, 0x04u, "");                 /* leaving the channel on the hardware trigger */
    GATE(0xFFu, 0x00u, "");                 /* HDRM_PRGM, THRSH1..3, MITIGATION_SW: no */
    /* the current ceiling the gate will sign, and the resolution's own range */
    GATE(0x7Fu, 0x17u, "43");
    GATE(0x7Fu, 0x18u, "");                 /* 24 > the 23 the DT's torch current gives */
    GATE(0x7Fu, 0x03u, "43 47");
    GATE(0x7Fu, 0x04u, "43");               /* ires_idx 4 does not exist */
    /* a ceiling re-derived from a wider hard current signs more codes and still nothing else */
    s.hard_code = 39u;
    GATE(0x7Fu, 0x27u, "43");
    GATE(0x7Fu, 0x28u, "");
    GATE(0x7Fu, 0x7Fu, "");
    s.hard_code = 23u;
    /* the interlock: the channel stays off until the gate has written all four itself */
    GATE(0x01u, 0x01u, "");
    const unsigned before = m.ch_cmds;
    CHECK(dbt_spmi_write(&s, DBT_R_EN_LED_CTRL, DBT_LED_MASK, DBT_V_ENABLE) == DBT_PERM);
    CHECK(s.refused == 1u && m.ch_cmds == before && m.log_n == 0u);
    CHECK(dbt_spmi_write(&s, DBT_R_IRES, DBT_M_CURRENT, 0u) == DBT_OK && s.have_ires == 1);
    CHECK(dbt_spmi_write(&s, DBT_R_STROBE_CTRL, DBT_M_STROBE_SEL, 0u) == DBT_OK && s.have_strobe == 1);
    GATE(0x01u, 0x01u, "");                 /* the current and the timer are still missing */
    CHECK(dbt_spmi_write(&s, DBT_R_TGR_CURRENT, DBT_M_CURRENT, 23u) == DBT_OK && s.have_current == 1);
    CHECK(dbt_spmi_write(&s, DBT_R_SAFETY_TMR, 0xFFu, DBT_V_TMR_DISABLE) == DBT_OK && s.have_timer == 1);
    GATE(0x01u, 0x01u, "4c");
    /* and the four bytes that did land are the vendor's own fields, in the vendor's own order */
    SEQ(0, "47=00 49=00 43=17 40=13");
    CLEAN();
}

static void read_modify_write(void)
{
    m_init(&m);
    DbtSpmiIo io = m_io(&m);
    memset(&s, 0, sizeof s);
    CHECK(dbt_spmi_discover(&s, &io, DBT_EE) == DBT_OK && dbt_spmi_open(&s, 23u) == DBT_OK);
    /* regmap_update_bits semantics: a bit outside the mask that the PMIC already held must survive.
     * bit 4 of STROBE_CTRL is not one the vendor's torch path names and bit 0 of MOD_CTRL is not
     * either; both are set here on purpose. */
    m.reg[DBT_R_STROBE_CTRL] = 0x14u;
    m.reg[DBT_R_MOD_CTRL] = 0x81u;
    s.have_ires = s.have_strobe = s.have_current = s.have_timer = 1;
    CHECK(dbt_spmi_write(&s, DBT_R_STROBE_CTRL, DBT_M_STROBE_SEL, 0u) == DBT_OK);
    CHECK(dbt_spmi_write(&s, DBT_R_MOD_CTRL, DBT_M_MOD_CTRL, DBT_V_MOD_ENABLE) == DBT_OK);
    CHECK(dbt_spmi_write(&s, DBT_R_EN_LED_CTRL, DBT_LED_MASK, DBT_V_ENABLE) == DBT_OK);
    SEQ(0, "49=10 46=81 4c=01");
    /* the stale code 0x7f and the stale 5 mA step the camera stack left add up to 640 mA, which is
     * over anything this board's torch node allows: the core's init and its four fields are what
     * keep a channel from ever being enabled on top of them */
    CHECK(m.lit == 1 && m.ma == 640u);
    /* switching the channel off clears the interlock: the next light-up must write the four bytes
     * again, because those registers are what the hardware uses */
    CHECK(dbt_spmi_write(&s, DBT_R_EN_LED_CTRL, DBT_LED_MASK, 0u) == DBT_OK);
    CHECK(s.have_ires == 0 && s.have_strobe == 0 && s.have_current == 0 && s.have_timer == 0);
    CHECK(dbt_spmi_allowed(&s, DBT_R_EN_LED_CTRL, DBT_LED_MASK, DBT_V_ENABLE) == 0);
    CHECK(m.lit == 0);
    CLEAN();
}

static void failures(void)
{
    m_init(&m);
    DbtSpmiIo io = m_io(&m);
    memset(&s, 0, sizeof s);
    CHECK(dbt_spmi_discover(&s, &io, DBT_EE) == DBT_OK && dbt_spmi_open(&s, 23u) == DBT_OK);
    s.have_ires = s.have_strobe = s.have_current = s.have_timer = 1;
    /* a transaction every shape of which the arbiter reports as failed is not a byte that landed,
     * even though the read half of the read-modify-write succeeded */
    all_write_shapes_fail();
    CHECK(dbt_spmi_write(&s, DBT_R_IRES, DBT_M_CURRENT, 0u) == DBT_ERROR);
    CHECK(m.log_n == 0u && s.writes == 0u && s.errors == 3u);
    CHECK(s.last_status == 0x3u && s.last_reg == DBT_R_IRES);
    CHECK(m.reg[DBT_R_IRES] == 0x03u);              /* the PMIC kept what it had */
    /* denied by the arbiter itself - the ownership changed under us - is a different status word,
     * kept distinct because spmi-pmic-arb.c keeps it distinct */
    m_init(&m);
    io = m_io(&m);
    memset(&s, 0, sizeof s);
    CHECK(dbt_spmi_discover(&s, &io, DBT_EE) == DBT_OK && dbt_spmi_open(&s, 23u) == DBT_OK);
    m.owner[M_AFLASH] = 4u;
    CHECK(dbt_spmi_write(&s, DBT_R_MOD_CTRL, DBT_M_MOD_CTRL, 0u) == DBT_ERROR);
    CHECK(s.last_status == 0x5u && m.log_n == 0u);
    CLEAN();
}

static void emergency(void)
{
    m_init(&m);
    DbtSpmiIo io = m_io(&m);
    memset(&s, 0, sizeof s);
    CHECK(dbt_spmi_discover(&s, &io, DBT_EE) == DBT_OK && dbt_spmi_open(&s, 23u) == DBT_OK);
    /* light it up the way the core would: the four fields, then the module, then the channel */
    CHECK(dbt_spmi_write(&s, DBT_R_IRES, DBT_M_CURRENT, 0u) == DBT_OK);
    CHECK(dbt_spmi_write(&s, DBT_R_STROBE_CTRL, DBT_M_STROBE_SEL, 0u) == DBT_OK);
    CHECK(dbt_spmi_write(&s, DBT_R_TGR_CURRENT, DBT_M_CURRENT, 23u) == DBT_OK);
    CHECK(dbt_spmi_write(&s, DBT_R_SAFETY_TMR, 0xFFu, DBT_V_TMR_DISABLE) == DBT_OK);
    CHECK(dbt_spmi_write(&s, DBT_R_MOD_CTRL, DBT_M_MOD_CTRL, DBT_V_MOD_ENABLE) == DBT_OK);
    CHECK(dbt_spmi_write(&s, DBT_R_EN_LED_CTRL, DBT_LED_MASK, DBT_V_ENABLE) == DBT_OK);
    CHECK(m.lit == 1 && m.ma == 300u);
    const unsigned n = m.log_n;
    /* the gate is closed - something refused, something failed - and the light is still on */
    s.closed = 1;
    CHECK(dbt_spmi_allowed(&s, DBT_R_MOD_CTRL, DBT_M_MOD_CTRL, 0u) == 0);
    CHECK(dbt_spmi_write(&s, DBT_R_MOD_CTRL, DBT_M_MOD_CTRL, DBT_V_MOD_DISABLE) == DBT_PERM);
    CHECK(s.refused == 1u && m.log_n == n);
    /* the stop is the one exception, and it goes in the vendor's order: the channel, then the module */
    CHECK(dbt_spmi_emergency_stop(&s) == DBT_OK);
    SEQ(n, "4c=00 46=00");
    CHECK(m.lit == 0 && m.ma == 0u && s.closed == 1);
    CHECK(s.have_ires == 0 && s.have_current == 0 && s.have_timer == 0 && s.have_strobe == 0);
    /* nothing after it is written, and a second stop is harmless */
    const unsigned after = m.log_n;
    CHECK(dbt_spmi_write(&s, DBT_R_IRES, DBT_M_CURRENT, 0u) == DBT_PERM);
    CHECK(dbt_spmi_emergency_stop(&s) == DBT_OK);
    CHECK(m.log_n == after + 2u && m.lit == 0);
    /* a stop onto a bus whose writes all fail is reported, not swallowed: the caller then knows the
     * light may still be on and says so in its own line */
    m_init(&m);
    io = m_io(&m);
    memset(&s, 0, sizeof s);
    CHECK(dbt_spmi_discover(&s, &io, DBT_EE) == DBT_OK && dbt_spmi_open(&s, 23u) == DBT_OK);
    all_write_shapes_fail();
    s.closed = 1;
    CHECK(dbt_spmi_emergency_stop(&s) == DBT_ERROR);
    CHECK(s.closed == 1 && m.log_n == 0u);
    /* and a stop the gate has no way to carry out is not a stop that happened: no bus under it, or a
     * bus with no channel found on it, are both the same answer - there is nothing to write to */
    DbtSpmi g;
    const unsigned sent = m.ch_cmds;
    memset(&g, 0, sizeof g);
    CHECK(dbt_spmi_emergency_stop(&g) == DBT_NODEV);
    CHECK(m.log_n == 0u && m.ch_cmds == sent);
    g.io = &io;                                      /* now there is a bus, and still no channel */
    g.apid = -1;
    CHECK(dbt_spmi_emergency_stop(&g) == DBT_NODEV);
    CHECK(m.ch_cmds == sent);
    CLEAN();
}

int main(void)
{
    codes();
    discovered();
    wrong_bus();
    preference();
    reads_first();
    ladder();
    whitelist();
    read_modify_write();
    failures();
    emergency();
    printf("PASS spmi: %u checks (four distinct answers, v5 discovery and the owned mapping, both "
           "ladders, the whitelist over all 256 bytes, the interlock, read-modify-write, failures, "
           "the one stop)\n", checks);
    return 0;
}
