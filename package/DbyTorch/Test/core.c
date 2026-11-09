/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host tests of DbyTorchCore.c on the arbiter + flash-module model: the boot arguments'
 * configuration, the current law including the PM8150L resolution erratum, the dry run, every
 * refusal that happens before a single write, the exact on and off byte sequences of
 * leds-qpnp-flash-v2.c, every level of the torch node's 300 mA, the module's own ceiling, the
 * 5-minute limit, a thermal derate that only comes down, every fault bit, a complete failure at
 * every write and every read of the sequences, a register that will not take the byte, and the
 * ration on the log. The model's LED says what the hardware would have done. */
#include "model.h"

static Model m;
static DbtCore t;
/* The core keeps the io record it is handed and uses it on every later call, so - unlike the local
 * DbtConfig of a bring-up - it has to live as long as the core does; the ops are static for the same
 * reason. A stack copy would leave the stored pointer pointing into a returned frame. */
static DbtSpmiIo g_io;

/* Nothing may ever hand the arbiter a command word or an offset it would not take. */
#define CLEAN() do { CHECK(m.bad_obs == 0 && m.bad_ch == 0); } while (0)

static const DbtOps *ops_of(void)
{
    static DbtOps o;
    o = m_ops(&m);
    return &o;
}

/* The bring-up of a model the caller has already prepared: the board's own ceilings, the argument's
 * max current (0 = the DT's 300 mA) and the log level, with everything else as the vendor's DT says. */
static int boot_model(unsigned mode, unsigned max_ma, unsigned log)
{
    DbtConfig c;
    CHECK(dbt_config_make(&c, mode, DBT_EE, DBT_TORCH_MAX_MA, max_ma, log) == 1);
    g_io = m_io(&m);
    return dbt_core_init(&t, &c, ops_of(), &g_io);
}

static void boot_ok(unsigned mode, unsigned max_ma, unsigned log, int expect)
{
    m_init(&m);
    CHECK(boot_model(mode, max_ma, log) == expect);
}

/* take over from a block that is already quiet: the init then writes nothing at all */
static void boot_quiet(void)
{
    m_init(&m);
    m.reg[DBT_R_EN_LED_CTRL] = 0x00u;
    m.reg[DBT_R_MOD_CTRL] = 0x00u;
    m_settle(&m);
    CHECK(boot_model(DBT_MODE_ON, 0u, 2u) == DBT_OK);
    CHECK(t.state == DBT_ST_OFF && t.stage == 5 && m.log_n == 0u);
    CHECK(t.spmi.reads == 20u && t.spmi.writes == 0u);
}

static void config(void)
{
    DbtConfig c;
    CHECK(dbt_config_make(0, DBT_MODE_ON, DBT_EE, 500, 300, 0) == 0);
    CHECK(dbt_config_make(&c, DBT_MODE_OFF, DBT_EE, 500, 300, 0) == 0);   /* no argument: no torch */
    CHECK(dbt_config_make(&c, 3, DBT_EE, 500, 300, 0) == 0);              /* not a mode this is */
    CHECK(dbt_config_make(&c, DBT_MODE_ON, DBT_EE, 500, 300, 0) == 1);
    CHECK(c.hard_ma == 500u && c.max_ma == 300u && c.ppid == DBT_PPID && c.led_mask == DBT_LED_MASK);
    CHECK(c.torch_id == DBT_TORCH_ID && c.ires_ua == DBT_TORCH_IRES_UA);
    CHECK(c.auto_off_ms == DBT_AUTO_OFF_MS && c.poll_ms == DBT_POLL_DEFAULT_MS && c.log == 0u);
    /* the boot argument may come down from the DT's two numbers and never up */
    CHECK(dbt_config_make(&c, DBT_MODE_ON, DBT_EE, 700, 900, 7) == 1 && c.hard_ma == 500u);
    CHECK(c.max_ma == 500u && c.log == 2u);
    /* a missing argument is that DT cell, not the other one: with neither, the port is exactly [D]'s
     * qcom,max-current = 500 and qcom,current-ma = 300 */
    CHECK(dbt_config_make(&c, DBT_MODE_ON, DBT_EE, 0, 0, 1) == 1 && c.hard_ma == 500u);
    CHECK(c.max_ma == 300u);
    CHECK(dbt_config_make(&c, DBT_MODE_ON, DBT_EE, 200, 500, 1) == 1 && c.hard_ma == 200u);
    CHECK(c.max_ma == 200u);
    CHECK(dbt_config_make(&c, DBT_MODE_DRY, DBT_EE, 500, 300, 0) == 1 && c.mode == DBT_MODE_DRY);
    /* the test argument is clamped: a "hold" that outruns the safety limit is not a hold, and the
     * clamp is the limit itself (dbt_test_ms is the glue's door for the hold= argument) */
    CHECK(dbt_test_ms(0) == 0 && dbt_test_ms(999u) == 999u);
    CHECK(dbt_test_ms(1000u) == 1000u && dbt_test_ms(1500u) == 1000u);
    CHECK(dbt_test_ms(0xFFFFFFFFu) == 1000u);
    CLEAN();
}

static void law(void)
{
    unsigned idx = 0, ires = 0, code = 0;
    /* the numbers this board's torch node gives: 300 mA at 12.5 mA, 500 mA is the module's ceiling */
    dbt_current_law(300u, DBT_TORCH_IRES_UA, 500u, &idx, &ires, &code);
    CHECK(idx == 0u && ires == 12500u && code == 23u);
    dbt_current_law(500u, DBT_TORCH_IRES_UA, 500u, &idx, &ires, &code);
    CHECK(code == 39u);
    dbt_current_law(200u, DBT_TORCH_IRES_UA, 500u, &idx, &ires, &code);
    CHECK(code == 15u && (code + 1u) * 12500u / 1000u == 200u);
    /* PM8150L_IRES_WA: at or below 20 mA the vendor steps on 5 mA, not on 12.5 mA */
    dbt_current_law(20u, DBT_TORCH_IRES_UA, 500u, &idx, &ires, &code);
    CHECK(idx == 3u && ires == 5000u && code == 3u);
    dbt_current_law(15u, DBT_TORCH_IRES_UA, 500u, &idx, &ires, &code);
    CHECK(idx == 3u && code == 2u);
    dbt_current_law(21u, DBT_TORCH_IRES_UA, 500u, &idx, &ires, &code);
    CHECK(idx == 0u && ires == 12500u);
    /* below the resolution step the current is the step, which is [V]:1349-1350's floor */
    dbt_current_law(5u, DBT_TORCH_IRES_UA, 500u, &idx, &ires, &code);
    CHECK(idx == 3u && code == 1u);
    dbt_current_law(0u, DBT_TORCH_IRES_UA, 500u, &idx, &ires, &code);
    CHECK(code == 0u);
    /* every current the module could be asked for, and every hard ceiling: a code never programs
     * more than one resolution step above what was asked, and never above the ceiling itself */
    for (unsigned cap = 12u; cap <= 500u; cap += 1u) {
        for (unsigned ma = 1u; ma <= 500u; ++ma) {
            dbt_current_law(ma, DBT_TORCH_IRES_UA, cap, &idx, &ires, &code);
            CHECK(idx == 0u || idx == 3u);            /* this board's ceiling never reaches the
                                                       * 640/960/1280 mA steps ([V]:1356-1366) */
            CHECK(ires == 12500u || ires == 5000u);
            CHECK(code <= 39u);
            const unsigned got = (code + 1u) * ires / 1000u;
            /* the ceiling and the floor both bind before the code is derived, so the number one
             * step above is compared against the current the module was programmed with, not
             * against the request: [V]:1349-1350 raises anything under ires_ua/1000 mA to that step. */
            unsigned want = ma < DBT_TORCH_IRES_UA / 1000u ? DBT_TORCH_IRES_UA / 1000u : ma;
            if (want > cap) want = cap;
            CHECK(got <= want + ires / 1000u);
            CHECK(got >= 5u);
            if (ma > 20u && cap > 20u) CHECK(idx == 0u);
        }
    }
    /* the level the OS hands down, and the vendor's floor */
    CHECK(dbt_level_ma(0u, 300u) == 0u);
    CHECK(dbt_level_ma(DBT_LEVEL_MAX, 300u) == 300u);
    CHECK(dbt_level_ma(1u, 300u) == 12u && dbt_level_ma(1u, 500u) == 12u);
    CHECK(dbt_level_ma(500u, 300u) == 150u);
    CHECK(dbt_level_ma(2000u, 300u) == 300u);         /* a level past the range is the range */
    unsigned previous = 0u;
    for (unsigned level = 1u; level <= DBT_LEVEL_MAX; ++level) {
        const unsigned ma = dbt_level_ma(level, 300u);
        CHECK(ma >= 12u && ma <= 300u && ma >= previous);
        previous = ma;
    }
}

static void dry_run(void)
{
    boot_ok(DBT_MODE_DRY, 0u, 2u, 1);
    CHECK(t.state == DBT_ST_DRY && t.stage == 5);
    /* the whole point: identity and registers, and not one byte written and no RW page mapped */
    CHECK(m.log_n == 0u && m.map_calls == 0u && m.ch_cmds == 0u);
    CHECK(t.spmi.reads == 16u && t.spmi.rw_mapped == 0);
    CHECK(m_logged(&m, "dry run done"));
    CHECK(m_logged(&m, "reg 08=00 09=00 10=00 40=13 43=7f 46=80 47=03 48=00 49=04 4c=01 4d=14"));
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "dry") == DBT_NODEV);
    CHECK(m.log_n == 0u);
    CHECK(dbt_core_poll(&t) == 0u);
    CLEAN();
}

static void refusals(void)
{
    /* a configuration the hardware would not accept stops before the bus is touched: 11 mA is below
     * the module's own 12.5 mA resolution step, so no code exists that programs it */
    m_init(&m);
    DbtConfig bad;
    CHECK(dbt_config_make(&bad, DBT_MODE_ON, DBT_EE, 11u, 300u, 2u) == 1);
    g_io = m_io(&m);
    CHECK(dbt_core_init(&t, &bad, ops_of(), &g_io) == -22);
    CHECK(t.stage == -1 && t.state == DBT_ST_REFUSED);
    CHECK(m.core_reads == 0u && m.log_n == 0u && m_logged(&m, "refused: config"));

    /* a v7 arbiter: the geometry this port would use is the wrong geometry, so nothing is read */
    m_init(&m);
    m.version = 0x70060000u;
    CHECK(boot_model(DBT_MODE_ON, 0u, 2u) == DBT_NODEV);
    CHECK(t.stage == -2 && t.state == DBT_ST_REFUSED);
    CHECK(m.core_reads == 1u && m.obs_cmds == 0u && m.log_n == 0u && t.spmi.reads == 0u);
    CHECK(m_logged(&m, "refused: arbiter"));

    /* no flash block in the APID table at all */
    m_init(&m);
    m.hide_flash_map = 1;
    CHECK(boot_model(DBT_MODE_ON, 0u, 2u) == DBT_NODEV);
    CHECK(t.stage == -2 && m_logged(&m, "rc=-19") && t.spmi.apid < 0);

    /* The write path is another EE's. The ownership table is consulted before the RW page is asked
     * for, because spmi-pmic-arb.c will not issue a write on a channel this EE does not own, and the
     * port stops with the same answer for the same reason: nothing of this module's has been touched. */
    m_init(&m);
    m.owner[M_AFLASH] = 1u;
    m.owner[M_AFOREIGN] = 2u;
    CHECK(boot_model(DBT_MODE_ON, 0u, 2u) == DBT_PERM);
    CHECK(t.stage == -4 && t.spmi.denied == 1u);
    CHECK(m.map_calls == 0u && m.ch_cmds == 0u && m.log_n == 0u && t.spmi.reads == 0u);
    CHECK(m_logged(&m, "refused: open"));

    /* a channel the glue cannot map */
    m_init(&m);
    m.chan_map_fail = 1;
    CHECK(boot_model(DBT_MODE_ON, 0u, 2u) == DBT_NODEV);
    CHECK(t.stage == -4 && m.map_calls == 1u && m.log_n == 0u && m_logged(&m, "refused: open"));

    /* A module that will not answer a read is not a module this port writes to. The read ladder has
     * already spent its three command words here, so nothing lands - and that is what the FAULT
     * line's stop=-5 says: the channel could not be taken down either, because every write of this
     * port reads the byte it is about to merge first. There is no blind-write fallback in
     * leds-qpnp-flash-v2.c to invent, so the honest answer is the number. */
    m_init(&m);
    m.rpol[0] = m.rpol[1] = m.rpol[2] = M_ERR;
    CHECK(boot_model(DBT_MODE_ON, 0u, 2u) == -5);
    CHECK(t.state == DBT_ST_FAULT && t.stage == 4 && t.spmi.closed == 1);
    CHECK(t.spmi.errors >= 3u && m.log_n == 0u && m.ch_cmds == 0u);
    CHECK(m_logged(&m, "FAULT init-read rc=-5 stop=-5"));
    /* the dry run takes the same read failure as a refusal, not a fault: it has written nothing */
    m_init(&m);
    m.rpol[0] = m.rpol[1] = m.rpol[2] = M_ERR;
    CHECK(boot_model(DBT_MODE_DRY, 0u, 2u) == DBT_ERROR);
    CHECK(t.state == DBT_ST_REFUSED && t.stage == -3 && m_logged(&m, "refused: dump-read"));

    /* a core that was never initialised is not a core that answers requests */
    DbtCore g;
    memset(&g, 0, sizeof g);
    CHECK(dbt_core_set(&g, 500u, 0u, "cold") == -22);
    CHECK(dbt_core_poll(&g) == 0u);
    CLEAN();
}

static void sequences(void)
{
    boot_ok(DBT_MODE_ON, 0u, 2u, DBT_OK);
    /* the boot itself: the light was on when this port took over, and the two clears are the only
     * writes it makes */
    CHECK(t.hard_code == 39 && t.code_max == 23);
    SEQ(0, "4c=00 46=00");
    CHECK(t.state == DBT_ST_OFF && t.stage == 5 && m.lit == 0);
    CHECK(t.spmi.reads == 22u && t.spmi.writes == 2u);
    CHECK(m_logged(&m, "ready") && m_logged(&m, "max_ma=300 hard_ma=500 code=23 hard_code=39"));
    const unsigned n = m.log_n;

    /* full brightness, in the vendor's order, with the board's own bytes */
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "control-center") == DBT_OK);
    SEQ(n, "47=00 49=00 43=17 40=13 46=80 4c=01");
    CHECK(m.lit == 1 && m.ma == 300u && t.state == DBT_ST_ON && t.ons == 1u);
    CHECK(t.ma == 300u && t.code == 23u && t.ires_idx == 0u);
    CHECK(m_logged(&m, "on level=1000 ma=300 code=23 ires=0 by=control-center"));
    /* the light is now armed with the hardware's own safety timer disabled, which is what the
     * software's 300 s deadline is for */
    CHECK((m.reg[DBT_R_SAFETY_TMR] & 0x80u) == 0u);

    /* a dimmer level re-programs through the same door: off, then the four fields, then on */
    const unsigned n2 = m.log_n;
    CHECK(dbt_core_set(&t, 500u, 0u, "slider") == DBT_OK);
    SEQ(n2, "4c=00 46=00 47=00 49=00 43=0b 40=13 46=80 4c=01");
    CHECK(m.ma == 150u && t.code == 11u);
    CHECK(m_logged(&m, "level level=500 ma=150 code=11"));

    /* the same level again is not a re-program */
    const unsigned writes = m.log_n;
    CHECK(dbt_core_set(&t, 500u, 0u, "slider-again") == DBT_OK);
    CHECK(m.log_n == writes && t.ons == 1u);

    /* the bottom of the range: the erratum's 5 mA step, and a current under what was asked */
    const unsigned n3 = m.log_n;
    CHECK(dbt_core_set(&t, 1u, 0u, "bottom") == DBT_OK);
    SEQ(n3, "4c=00 46=00 47=03 49=00 43=01 40=13 46=80 4c=01");
    CHECK(m.ma == 10u && t.ma == 12u && t.ires_idx == 3u);
    CHECK(m.lit == 1);

    /* off, in the vendor's order: the channel, the module, and only then the fields put away */
    const unsigned n4 = m.log_n;
    CHECK(dbt_core_set(&t, 0u, 0u, "control-center") == DBT_OK);
    SEQ(n4, "4c=00 46=00 43=00 40=13");
    CHECK(m.lit == 0 && m.ma == 0u && t.state == DBT_ST_OFF && t.offs == 1u);
    CHECK(m_logged(&m, "off level=0 ma=0 code=0"));
    /* off twice is off */
    const unsigned after = m.log_n;
    CHECK(dbt_core_set(&t, 0u, 0u, "again") == DBT_OK);
    CHECK(m.log_n == after && t.offs == 1u);
    /* and a request from a quiet core does not touch the bus at all */
    CHECK(dbt_core_poll(&t) == 0u);
    CHECK(m.log_n == after);

    /* every level the OS can name, on the real model: the interlock makes the gate sign each one,
     * and nothing anywhere in the range draws more than the torch node's 300 mA */
    static const unsigned levels[] = {1u, 2u, 5u, 10u, 20u, 21u, 40u, 50u, 80u, 100u, 150u,
                                      200u, 250u, 300u, 400u, 500u, 600u, 700u, 800u, 900u,
                                      DBT_LEVEL_MAX};
    for (unsigned i = 0; i < sizeof levels / sizeof levels[0]; ++i) {
        CHECK(dbt_core_set(&t, levels[i], 0u, "sweep") == DBT_OK);
        CHECK(m.lit == 1 && m.ma <= 300u);
        CHECK(t.code <= (unsigned)t.code_max && t.ma == dbt_level_ma(levels[i], 300u));
        CHECK(dbt_core_set(&t, 0u, 0u, "sweep") == DBT_OK);
        CHECK(m.lit == 0);
    }
    /* the resolution the gate signed is the resolution the hardware took */
    CHECK(m.reg[DBT_R_EN_LED_CTRL] == 0x00u && m.reg[DBT_R_MOD_CTRL] == 0x00u);
    const unsigned quiet = m.log_n;
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX + 1u, 0u, "past") == -22);
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX * 2u, 0u, "past") == -22);
    CHECK(dbt_core_set(&t, 0u, 0u, "quiet") == DBT_OK);         /* off, from a core already off */
    CHECK(m.log_n == quiet);                                    /* and none of them touched the bus */
    CLEAN();
}

static void deadlines(void)
{
    boot_quiet();
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") == DBT_OK);
    CHECK(t.deadline_us == m.now_us + (uint64_t)DBT_AUTO_OFF_MS * 1000u);
    /* the poll runs at the configured rate and the light is still on well inside the limit */
    m_run(&m, &t, 100000000ull);                   /* 100 s, in the microseconds m_run counts in */
    CHECK(t.state == DBT_ST_ON && m.lit == 1);
    CHECK(t.offs == 0u);
    /* past the limit the port turns it off by itself and then stops polling */
    m_run(&m, &t, 205000000ull);
    CHECK(t.state == DBT_ST_OFF && t.offs == 1u && m.lit == 0);
    /* the line says the limit it obeyed, not just that it stopped: 300 s on the wall clock */
    CHECK(m_logged(&m, "auto-off level=0 ma=0 code=0 ires=0 by=deadline on_ms=300000 ons=1"));
    CHECK(dbt_core_poll(&t) == 0u);

    /* a shorter hold is honoured */
    const unsigned n = m.log_n;
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 1000u, "scan") == DBT_OK);
    SEQ(n, "47=00 49=00 43=17 40=13 46=80 4c=01");
    CHECK(t.deadline_us == m.now_us + 1000000ull);
    m_run(&m, &t, 1100000ull);
    CHECK(t.state == DBT_ST_OFF && m.lit == 0);

    /* and a longer one is not: a request may only shorten the safety limit, never outrun it */
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") == DBT_OK);
    const uint64_t dl = t.deadline_us;
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 400000u, "greedy") == DBT_OK);
    CHECK(t.deadline_us == dl);
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 60000u, "shorter") == DBT_OK);
    CHECK(t.deadline_us == m.now_us + 60000000ull);
    CLEAN();
}

static void thermal(void)
{
    /* [D] qcom,thermal-derate-en with <200 500 1000>: heat derates, it does not stop the light */
    boot_ok(DBT_MODE_ON, 0u, 2u, DBT_OK);
    m_fault_now(&m, 0u, 0x01u, 0u);                 /* OTST level 1, already true at the first read */
    const unsigned n = m.log_n;
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") == DBT_OK);
    SEQ(n, "47=00 49=00 43=17 40=13 46=80 4c=01 43=0f");
    CHECK(t.state == DBT_ST_ON && m.lit == 1 && m.ma == 200u);
    CHECK(t.derates == 1u && t.code == 15u);
    CHECK(m_logged(&m, "derate otst=1 ma=200 code=15 derates=1"));
    /* the next poll sees the same level and writes nothing: a derate is not a flap */
    const unsigned after = m.log_n;
    CHECK(dbt_core_poll(&t) == t.cfg.poll_ms);
    CHECK(m.log_n == after && t.derates == 1u);
    /* and a derate never raises a current that is already lower */
    CHECK(dbt_core_set(&t, 0u, 0u, "off") == DBT_OK);
    CHECK(dbt_core_set(&t, 500u, 0u, "slider") == DBT_OK);      /* 150 mA, under the 200 mA limit */
    CHECK(t.derates == 1u && m.ma == 150u);
    /* levels 2..5 derate to 500 mA and 1000 mA, both above this torch's own 300 mA: no reprogram */
    for (unsigned otst = 2u; otst <= 5u; ++otst) {
        m.fault_s2 = (uint8_t)otst;
        const unsigned before = m.log_n;
        CHECK(dbt_core_poll(&t) != 0u || t.state != DBT_ST_ON);
        CHECK(m.log_n == before && t.derates == 1u);
    }
    /* levels 6 and 7 are outside the DT's three cells. The vendor's driver would read one past them
     * (leds-qpnp-flash-v2.c:1290-1318 indexes thermal_derate_current[otst >> 1] with otst up to 7);
     * this port clamps to the last cell, and here the clamp is what keeps the read in bounds. */
    for (unsigned otst = 6u; otst <= 7u; ++otst) {
        m.fault_s2 = (uint8_t)otst;
        CHECK(dbt_core_poll(&t) == t.cfg.poll_ms);           /* the light stays on and keeps polling */
        CHECK(t.derates == 1u);
        CHECK(t.state != DBT_ST_FAULT);                    /* a hot PMIC is not a broken one */
    }
    CLEAN();
}

static void faults(void)
{
    /* STATUS1 any bit *with the vendor's own condition met*: [V]:1992-2009 reads the byte inside the
     * led-fault-irq branch, and the board DTs name INT_RT_STS bit 0 that interrupt, so the fixture
     * raises both. The answer is the light out through switch_disable - the enable bit, the module
     * bit, the target current and the disabled timer, in [V]'s order - and an open gate. */
    static const uint8_t s1[] = {0x01u, 0x02u, 0x04u, 0x10u, 0x80u, 0xFFu};
    for (unsigned i = 0; i < sizeof s1 / sizeof s1[0]; ++i) {
        boot_ok(DBT_MODE_ON, 0u, 2u, DBT_OK);
        m_fault_now(&m, s1[i], 0u, DBT_V_LED_FAULT);
        CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") == -5);
        CHECK(t.state == DBT_ST_OFF && t.spmi.closed == 0 && m.lit == 0);
        CHECK(m.reg[DBT_R_EN_LED_CTRL] == 0u && m.reg[DBT_R_MOD_CTRL] == 0u);
        CHECK(m.reg[DBT_R_TGR_CURRENT] == 0u && m.reg[DBT_R_SAFETY_TMR] == (uint8_t)DBT_TORCH_TMR);
        CHECK(m_logged(&m, "FAULT short-or-open"));
        /* and the next request is served: a lamp fault is not a reason to lose the torch for the boot */
        m.fault_s1 = 0u;
        m.fault_armed = 0;
        CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "again") == DBT_OK);
        CHECK(t.state == DBT_ST_ON && m.lit == 1 && t.faults == 1u);
        CLEAN();
    }
    /* STATUS2 bit 4 is the VPH droop, which [D] enables on this board - under the same fault line */
    boot_ok(DBT_MODE_ON, 0u, 2u, DBT_OK);
    m_fault_now(&m, 0u, DBT_V_VPH_DROOP, DBT_V_LED_FAULT);
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") == -5);
    CHECK(t.state == DBT_ST_OFF && m.lit == 0);
    CHECK(m_logged(&m, "FAULT vph-droop"));
    /* a fault that arrives while the light is already on stops it on the next poll */
    boot_ok(DBT_MODE_ON, 0u, 2u, DBT_OK);
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") == DBT_OK);
    m.fault_s1 = 0x01u;
    m.fault_s3 = DBT_V_LED_FAULT;
    m.fault_armed = 1;                              /* and the clock does move here: m_run below */
    m.fault_at_us = m.now_us + 500000ull;                  /* half a second from now */
    m_run(&m, &t, 1000000ull);
    CHECK(t.state == DBT_ST_OFF && m.lit == 0 && t.faults == 1u);
    /* the interrupt-status byte with no status behind it is [V] printing nothing, so it stops nothing,
     * and ord214's real reading is the same rule the other way: a ramp-up STATUS1 with bit 0 clear -
     * STATUS1=01, INT_RT_STS=90, the fault line low - must not put a working light out */
    boot_ok(DBT_MODE_ON, 0u, 2u, DBT_OK);
    m_fault_now(&m, 0x01u, 0u, 0x90u);
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") == DBT_OK);
    CHECK(t.state == DBT_ST_ON && m.lit == 1 && t.faults == 0u);
    CHECK(dbt_core_poll(&t) == t.cfg.poll_ms);
    CLEAN();
    boot_ok(DBT_MODE_ON, 0u, 2u, DBT_OK);
    m_fault_now(&m, 0u, 0u, 0xFFu);                 /* the whole interrupt byte, no status detail */
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") == DBT_OK);
    CHECK(t.state == DBT_ST_ON && m.lit == 1 && t.faults == 0u);
    CHECK(dbt_core_poll(&t) == t.cfg.poll_ms);
    CLEAN();
}

/* The run every failure test walks: take over the block the camera stack left enabled, light the
 * torch at full and put it away. That is 12 transactions on the request channel and 48 on the
 * observer. The 48 reads are 22 in the bring-up - the two state reads, the two merges the clears
 * travel through, their two read-backs and the sixteen bytes of the identity dump - 17 in the
 * light-up and 9 in the switch-disable. Every one of them is either a byte the port merges a write
 * into or a byte it verifies, which is why a read that does not answer has to stop the run. */
static void standard_run(int *rc0, int *rc1, int *rc2)
{
    *rc0 = boot_model(DBT_MODE_ON, 0u, 2u);
    *rc1 = (*rc0 == DBT_OK) ? dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") : -5;
    *rc2 = (*rc1 == DBT_OK) ? dbt_core_set(&t, 0u, 0u, "cc") : -5;
}

static const char *write_site(unsigned k)
{
    if (k < 2u) return "FAULT init-write";            /* the two clears of the take-over */
    if (k < 6u) return "FAULT on-write";              /* resolution, strobe, current, timer */
    if (k == 6u) return "FAULT module-write";
    if (k == 7u) return "FAULT enable-write";
    return "FAULT off-write";                         /* the four bytes of switch_disable */
}

/* One whole ladder of the write side, at every write the run makes: the arbiter answers DONE with
 * an error bit on all three command words, which is spmi-pmic-arb.c's -EIO and not a retry this
 * port may walk past. The light must never be left on behind a byte that did not land. */
static void write_failure_at(void)
{
    for (unsigned k = 0u; k < 12u; ++k) {
        m_init(&m);
        m.fail_w_txn = (int)k;
        int rc0, rc1, rc2;
        standard_run(&rc0, &rc1, &rc2);
        CHECK(rc0 == (k < 2u ? -5 : DBT_OK));
        CHECK(rc1 == (k < 8u ? -5 : DBT_OK));
        CHECK(rc2 == -5);                             /* nothing is written after a fault */
        CHECK(t.state == DBT_ST_FAULT && t.spmi.closed == 1 && t.faults == 1u);
        CHECK(t.spmi.errors >= 3u && m_logged(&m, write_site(k)));
        /* the port's last act is always the vendor's own switch-disable, and a bus that answers on
         * one of its three words is enough for it to get the channel and the module down */
        CHECK(m.reg[DBT_R_EN_LED_CTRL] == 0x00u && m.reg[DBT_R_MOD_CTRL] == 0x00u);
        CHECK(m.lit == 0 && m.ma == 0u);
        CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "after") == -5);
        CHECK(dbt_core_poll(&t) == 0u);
        CLEAN();
    }
}

static const char *read_site(unsigned k)
{
    if (k < 2u) return "FAULT init-read";             /* what the take-over reads before it clears */
    if (k < 4u) return "FAULT init-write";            /* a write merges the byte it is about to set */
    if (k < 6u) return "FAULT init-verify-read";
    if (k < 22u) return "FAULT dump-read";            /* the evidence of the module's own bytes */
    if (k < 24u) return "FAULT on-read";
    if (k < 28u) return "FAULT on-write";
    if (k < 32u) return "FAULT verify-read";           /* the four fields, read back */
    if (k < 34u) return "FAULT module-write";
    if (k == 34u) return "FAULT enable-write";
    if (k == 35u) return "FAULT enable-read";
    if (k < 39u) return "FAULT status-read";           /* the three bytes the faults come from */
    if (k < 41u) return "FAULT off-read";
    if (k < 45u) return "FAULT off-write";
    return "FAULT off-read";                           /* the three read-backs that say it is out */
}

/* The same on the read side, transaction by transaction: a byte the arbiter will not hand over is
 * not a byte this port may assume, so every one of these 48 stops the run. The site names follow
 * from the numbering in the comment above standard_run, and they are the answer to "which read was
 * this port relying on here" - a merge under a masked write names the write, because that is what
 * did not happen. */
static void read_failure_at(void)
{
    for (unsigned k = 0u; k < 48u; ++k) {
        m_init(&m);
        m.fail_r_txn = (int)k;
        int rc0, rc1, rc2;
        standard_run(&rc0, &rc1, &rc2);
        CHECK(rc0 == (k < 22u ? -5 : DBT_OK));
        CHECK(rc1 == (k < 39u ? -5 : DBT_OK));
        CHECK(rc2 == -5);
        CHECK(t.state == DBT_ST_FAULT && t.spmi.closed == 1 && t.faults == 1u);
        CHECK(t.spmi.errors >= 3u && m_logged(&m, read_site(k)));
        CHECK(m.reg[DBT_R_EN_LED_CTRL] == 0x00u && m.reg[DBT_R_MOD_CTRL] == 0x00u);
        CHECK(m.lit == 0);
        /* the run is over: the core answers every later request from its own state */
        CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "after") == -5);
        CHECK(dbt_core_poll(&t) == 0u);
        CLEAN();
    }
}

/* A single transaction that answers with an error bit is not a failure, and that is what the ladder
 * is for: whichever transaction of the run goes wrong, the byte still lands, the sequence is still
 * the vendor's and nothing faults. */
static void transient_write(void)
{
    for (unsigned k = 0u; k < 12u; ++k) {
        m_init(&m);
        m.fail_writes_after = (int)k;
        int rc0, rc1, rc2;
        standard_run(&rc0, &rc1, &rc2);
        CHECK(rc0 == DBT_OK && rc1 == DBT_OK && rc2 == DBT_OK);
        CHECK(t.state == DBT_ST_OFF && t.faults == 0u && t.spmi.closed == 0);
        CHECK(t.spmi.writes == 12u && t.spmi.errors >= 1u && t.spmi.retries >= 1u);
        SEQ(0, "4c=00 46=00 47=00 49=00 43=17 40=13 46=80 4c=01 4c=00 46=00 43=00 40=13");
        CHECK(m.lit == 0 && m.ma == 0u);
        CLEAN();
    }
}

static void transient_read(void)
{
    for (unsigned k = 0u; k < 48u; ++k) {
        m_init(&m);
        m.fail_reads_after = (int)k;
        int rc0, rc1, rc2;
        standard_run(&rc0, &rc1, &rc2);
        CHECK(rc0 == DBT_OK && rc1 == DBT_OK && rc2 == DBT_OK);
        CHECK(t.state == DBT_ST_OFF && t.faults == 0u);
        CHECK(t.spmi.reads == 48u && t.spmi.writes == 12u && t.spmi.errors >= 1u);
        SEQ(0, "4c=00 46=00 47=00 49=00 43=17 40=13 46=80 4c=01 4c=00 46=00 43=00 40=13");
        CLEAN();
    }
}

static void stuck_registers(void)
{
    /* the arbiter acknowledges the byte and the PMIC keeps the old one: the read-back after the
     * write is what turns that into a refusal instead of a light at a current nobody asked for */
    static const struct { uint8_t reg; const char *line; } cases[] = {
        {DBT_R_IRES,        "FAULT verify-ires"},
        {DBT_R_STROBE_CTRL, "FAULT verify-strobe"},
        {DBT_R_TGR_CURRENT, "FAULT verify-current"},
        {DBT_R_SAFETY_TMR,  "FAULT verify-timer"},
        {DBT_R_MOD_CTRL,    "FAULT module-verify"},
        {DBT_R_EN_LED_CTRL, "FAULT verify-enable"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        boot_quiet();
        /* the boot values must differ from what the port writes, or the ignored byte would still
         * read back as the value the port wanted */
        m.reg[DBT_R_IRES] = 0x03u;
        m.reg[DBT_R_TGR_CURRENT] = 0x7Fu;
        m.reg[DBT_R_STROBE_CTRL] = DBT_M_STROBE_SEL;
        m.reg[DBT_R_SAFETY_TMR] = 0x00u;
        m.stuck_on = 1;
        m.stuck_reg = cases[i].reg;
        CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") == -5);
        CHECK(t.state == DBT_ST_FAULT && m.lit == 0 && t.spmi.closed == 1);
        CHECK(m_logged(&m, cases[i].line));
        CLEAN();
    }
    /* A byte the arbiter acknowledges and the PMIC keeps is caught at the take-over too, before any
     * current is asked for, and in either of the two bits the light is made of. The stop then still
     * reaches the register that is not stuck, and the module down is what puts the light out. */
    static const uint8_t boot_stuck[] = {DBT_R_MOD_CTRL, DBT_R_EN_LED_CTRL};
    for (unsigned i = 0u; i < sizeof boot_stuck / sizeof boot_stuck[0]; ++i) {
        m_init(&m);
        m.stuck_on = 1;
        m.stuck_reg = boot_stuck[i];
        CHECK(boot_model(DBT_MODE_ON, 0u, 2u) == -5);
        CHECK(t.state == DBT_ST_FAULT && t.stage == 4);
        CHECK(m_logged(&m, "FAULT init-verify"));
        CHECK(m.lit == 0);
        CLEAN();
    }
    /* A status read that stops answering while the light is on is a fault too, not a silent ok - and
     * the stop that follows is a write, so the channel really does come down. */
    boot_quiet();
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") == DBT_OK);
    CHECK(m.lit == 1);
    m.fail_r_txn = (int)m.obs_cmds;                    /* the next transaction is the one that dies */
    CHECK(dbt_core_poll(&t) == 0u);
    CHECK(t.state == DBT_ST_FAULT && t.spmi.closed == 1);
    CHECK(m_logged(&m, "FAULT status-read"));
    CHECK(m.reg[DBT_R_EN_LED_CTRL] == 0x00u && m.reg[DBT_R_MOD_CTRL] == 0x00u);
    CHECK(m.lit == 0);
    CLEAN();
    /* A bus that answers no read at all is the one case this port cannot put the light out on, and
     * the test says so instead of looking away: every write here is a masked write, a masked write
     * reads the byte it is about to merge first ([V]:427-437) and there is no blind whole-byte
     * fallback in leds-qpnp-flash-v2.c to invent. What the port owes the operator instead is the
     * number, which is why the FAULT line carries stop=-5 and not a silent success. */
    boot_quiet();
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") == DBT_OK);
    m.rpol[0] = m.rpol[1] = m.rpol[2] = M_ERR;
    CHECK(dbt_core_poll(&t) == 0u);
    CHECK(t.state == DBT_ST_FAULT && t.spmi.closed == 1);
    CHECK(m_logged(&m, "FAULT status-read rc=-5 stop=-5"));
    CHECK(m.lit == 1);                                 /* still on - the line above is the warning */
    CLEAN();
}

static void logging(void)
{
    /* log=0: no event lines at all, and the four bring-up lines are all anybody ever sees */
    boot_ok(DBT_MODE_ON, 0u, 0u, DBT_OK);
    const unsigned lines = m.line_n;
    CHECK(lines == 4u);
    for (unsigned i = 0u; i < 20u; ++i) {
        CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") == DBT_OK);
        CHECK(dbt_core_set(&t, 0u, 0u, "cc") == DBT_OK);
    }
    CHECK(m.line_n == lines && t.event_lines == 0u && t.dropped == 40u);

    /* log=1 (the default): the first eight events, then one per 300 s */
    boot_ok(DBT_MODE_ON, 0u, 1u, DBT_OK);
    for (unsigned i = 0u; i < 30u; ++i) {
        CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") == DBT_OK);
        CHECK(dbt_core_set(&t, 0u, 0u, "cc") == DBT_OK);
    }
    CHECK(t.event_lines == 8u);
    CHECK(t.dropped == 60u - 8u);
    const unsigned before = m.line_n;
    m_advance(&m, 300000001ull);                    /* one microsecond past the core's own 300 s gap */
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") == DBT_OK);
    CHECK(m.line_n == before + 1u && t.event_lines == 9u);
    /* the line the ration lets through is the honest one: it carries what the ration swallowed */
    CHECK(m_logged(&m, "dropped=52"));
    CHECK(dbt_core_set(&t, 0u, 0u, "cc") == DBT_OK);
    CHECK(m.line_n == before + 1u);                 /* still inside the same gap */

    /* log=2: everything, up to a hard cap that a stuck client cannot get past */
    boot_ok(DBT_MODE_ON, 0u, 2u, DBT_OK);
    const unsigned cap_from = m.line_n;
    for (unsigned i = 0u; i < 40u; ++i) {
        CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") == DBT_OK);
        CHECK(dbt_core_set(&t, 0u, 0u, "cc") == DBT_OK);
    }
    CHECK(t.event_lines == 64u);
    CHECK(t.dropped == 80u - 64u);
    /* The cap is the end of the log, not a slower log: past 64 event lines nothing more is printed,
     * so the dropped count is in the core's state and in no line. Only the rationed level (log=1)
     * lets later lines through, and only they carry dropped=. */
    CHECK(m.line_n == cap_from + t.event_lines);
    CHECK(!m_logged(&m, "dropped="));
    CLEAN();
}

static void exported(void)
{
    boot_quiet();
    CHECK(dbt_core_set(&t, DBT_LEVEL_MAX, 0u, "cc") == DBT_OK);
    CHECK(dbt_core_set(&t, 0u, 0u, "cc") == DBT_OK);
    int32_t out[16];
    dbt_core_export(&t, out);
    CHECK(out[0] == (int32_t)t.state && out[0] == DBT_ST_OFF);
    CHECK(out[1] == 5 && out[2] == 0 && out[3] == 0 && out[4] == 0 && out[5] == 0);
    CHECK(out[6] == 0 && out[7] == 1 && out[8] == 1 && out[9] == 0);
    CHECK(out[12] == 0 && out[13] == 0);                /* nothing refused, no transaction failed */
    CHECK(out[14] == (int32_t)M_AFLASH && out[15] == (int32_t)t.spmi.last_status);
    /* the numbers the glue shows on the console are the ones the run produced, and the run produced
     * 20 reads of a quiet take-over, 17 of the light-up and 9 of the switch-disable against 6 + 4
     * writes - so a console line that says 46/10 is telling the truth about this hardware */
    CHECK(out[10] == (int32_t)t.spmi.reads && out[10] == 46);
    CHECK(out[11] == (int32_t)t.spmi.writes && out[11] == 10);
    CLEAN();
}

int main(void)
{
    config();
    law();
    dry_run();
    refusals();
    sequences();
    deadlines();
    thermal();
    faults();
    write_failure_at();
    read_failure_at();
    transient_write();
    transient_read();
    stuck_registers();
    logging();
    exported();
    printf("PASS core: %u checks (configuration, the current law with the PM8150L erratum, dry run, "
           "refusals before any write, the vendor's on and off sequences, every level of 300 mA, "
           "the 5 minute limit, the thermal derate, every fault bit, a dead ladder at each of the "
           "12 writes and each of the 48 reads, one transient error at each of them, a stuck "
           "register, the log's ration)\n", checks);
    return 0;
}
