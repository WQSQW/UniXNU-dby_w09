/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* DBY-W09 (SM8250/kona) flashlight facts. Every number here has one of two sources and cites it:
 * [V] = the vendor's own driver for this module, leds-qpnp-flash-v2.c (the compatible string of this
 *       board's DTBO node is qcom,qpnp-flash-led-v2, so that file - not leds-qcom-flash.c - is the
 *       register manual for this hardware), and
 * [D] = this unit's stock DTBO dump (fragment@4/__overlay__/qcom,pm8150l@5/qcom,leds@d300/...) plus
 *       arch/arm64/boot/dts/vendor/qcom/kona.dtsi for the SPMI arbiter windows.
 * The module is PM8150L's flash block: USID 5, block address 0xd300 ([D] reg = <0xd300 0x100>), so
 * the PPID the arbiter table carries is (5 << 8) | 0xd3 = 0x5d3. Register addresses below are the
 * byte offsets inside that 256-byte block, because in v5 the block address selects the APID and the
 * command word carries only the low byte (spmi-pmic-arb.c:977/:996, and DbyBattery.cpp's uint8_t
 * addresses, which is the shape this board already answers on). */
#ifndef DBY_TORCH_REGS_H
#define DBY_TORCH_REGS_H

#include <stdint.h>

/* ---- the SPMI v5 arbiter, verbatim from DbyBattery.cpp (the accepted access on this board) ---- */
#define DBT_ARB_CORE          0x0C440000ull   /* [D] kona.dtsi:3484 "core" */
#define DBT_ARB_CORE_SZ       0x1100u
#define DBT_ARB_CNFG          0x0C40A000ull   /* [D] kona.dtsi:3488 "cnfg"; one page covers the table */
#define DBT_ARB_CNFG_SZ       0x1000u
#define DBT_ARB_OBS           0x0E600000ull   /* [D] kona.dtsi:3527 "obsrvr" */
#define DBT_ARB_OBS_SZ        0x100000u
#define DBT_ARB_CHNLS         0x0C600000ull   /* [D] kona.dtsi:3526 "chnls" */
#define DBT_ARB_CHNLS_REGION  0x2000000ull
#define DBT_ARB_PAGE          0x10000u        /* 0x10000 * apid, both sides (spmi-pmic-arb.c:1000-1013) */
#define DBT_ARB_CHNL_SZ       0x100u          /* CMD 0x00 .. WDATA1 0x14 */
#define DBT_ARB_OBS_EE_STRIDE 0x10000u
#define DBT_ARB_OBS_APID_STRIDE 0x80u

#define DBT_ARB_REG_VERSION   0x0000u         /* PMIC_ARB_VERSION */
#define DBT_ARB_V5_MIN        0x50000000u
#define DBT_ARB_V5_MAX        0x70000000u     /* exclusive */
#define DBT_ARB_APID_MAP      0x900u          /* 0x900 + 4*n, pmic_arb_apid_map_offset_v5 */
#define DBT_ARB_OWNERSHIP     0x700u          /* SPMI_OWNERSHIP_TABLE_REG */
#define DBT_ARB_PPID_SHIFT    8u
#define DBT_ARB_PPID_MASK     0xFFFu
#define DBT_ARB_IRQ_OWNER_BIT (1u << 24)
#define DBT_ARB_OWNER_MASK    0x7u
#define DBT_ARB_MAX_APIDS     512u

#define DBT_ARB_CMD           0x00u
#define DBT_ARB_STATUS        0x08u
#define DBT_ARB_WDATA0        0x10u
#define DBT_ARB_RDATA0        0x18u
#define DBT_STS_DONE          (1u << 0)
#define DBT_STS_ERROR_MASK    0xEu            /* FAILURE|DENIED|DROPPED */
#define DBT_POLL_COUNT        1000u           /* PMIC_ARB_TIMEOUT_US, one udelay(1) per read */

/* The command word: opcode<<27 | addr<<4 | (sid<<20 when the form carries it) | (len-1). One byte is
 * len-1 == 0, so there is no length term anywhere in this port. */
#define DBT_CMD_OP_SHIFT      27u
#define DBT_CMD_ADDR_SHIFT    4u
#define DBT_CMD_SID_SHIFT     20u

/* The read ladder and the write ladder, in the order DbyBattery.cpp proved on this tablet. */
#define DBT_READ_FORMS   {{1u, 0}, {15u, 0}, {13u, 1}}
#define DBT_WRITE_FORMS  {{0u, 0}, {14u, 0}, {2u, 1}}
#define DBT_FORM_COUNT   3u

/* ---- this board's PMIC / peripheral identity ---- */
#define DBT_USID            5u                /* [D] qcom,pm8150l@5 */
#define DBT_PID             0xD3u             /* [D] qcom,leds@d300 */
#define DBT_PPID            ((DBT_USID << 8) | DBT_PID)   /* 0x5d3 */
#define DBT_EE              0u                /* [D] kona.dtsi `qcom,ee = <0>` */

/* ---- the flash-v2 module registers ([V]:29-58, [V]:116-129) ---- */
#define DBT_R_SUBTYPE       0x05u             /* [V]:29; the v2 driver never validates a TYPE byte */
#define DBT_R_TYPE          0x04u             /* evidence only: nothing in [V] reads or checks it */
#define DBT_R_STATUS1       0x08u             /* [V]:31 */
#define DBT_R_STATUS2       0x09u             /* [V]:33 */
#define DBT_R_INT_RT_STS    0x10u             /* [V]:37 */
#define DBT_R_SAFETY_TMR    0x40u             /* [V]:39, per channel at + id */
#define DBT_R_TGR_CURRENT   0x43u             /* [V]:42, per channel at + id */
#define DBT_R_MOD_CTRL      0x46u             /* [V]:44 */
#define DBT_R_IRES          0x47u             /* [V]:48 */
#define DBT_R_STROBE_CFG    0x48u             /* [V]:50 */
#define DBT_R_STROBE_CTRL   0x49u             /* [V]:53, per channel at + id */
#define DBT_R_EN_LED_CTRL   0x4Cu             /* [V]:57 */
#define DBT_R_HDRM_PRGM     0x4Du             /* [V]:60, per channel at + id; [V]:614 probe-time only */
#define DBT_R_THRSH1        0x56u             /* [V]:78 */
#define DBT_R_THRSH2        0x57u             /* [V]:81 */
#define DBT_R_THRSH3        0x58u             /* [V]:82 */
#define DBT_R_MITIGATION_SW 0x6Fu             /* [V]:120 */
#define DBT_R_MULTI_STROBE  0x71u             /* [V]:128 */

#define DBT_M_CURRENT       0x7Fu             /* FLASH_LED_CURRENT_MASK, GENMASK(6,0) */
#define DBT_M_MOD_CTRL      0x80u             /* FLASH_LED_MOD_CTRL_MASK = BIT(7) */
#define DBT_V_MOD_ENABLE    0x80u             /* FLASH_LED_MOD_ENABLE = BIT(7) */
#define DBT_V_MOD_DISABLE   0x00u
#define DBT_M_STROBE_SEL    0x04u             /* FLASH_LED_HW_SW_STROBE_SEL_BIT = BIT(2), the SW case */
#define DBT_M_THERMAL_THRSH 0x07u             /* GENMASK(2,0) */
#define DBT_M_OTST          0x07u             /* FLASH_LED_THERMAL_OTST_MASK in STATUS2 */
#define DBT_V_VPH_DROOP     0x10u             /* FLASH_LED_VPH_DROOP_FAULT_MASK = BIT(4) in STATUS2 */
#define DBT_V_LED_FAULT     0x01u             /* [V]:1992,2843: the led-fault-irq the two status bytes are only read under; every recorded board's DT puts it at INT_RT_STS bit 0 */
#define DBT_V_TMR_DISABLE   0x13u             /* FLASH_LED_SAFETY_TMR_DISABLED: bit7 clear, code 1 */
#define DBT_V_ENABLE        0x01u             /* FLASH_LED_ENABLE = BIT(0), shifted by the channel id */

/* ---- the current law of the module ([V]:177-181, :369-380, :1339-1387) ---- */
#define DBT_IRES_BASE       3u                /* FLASH_LED_IRES_BASE */
#define DBT_IRES_DIVISOR    2500u             /* FLASH_LED_IRES_DIVISOR */
#define DBT_IRES_MIN_UA     5000u             /* FLASH_LED_IRES_MIN_UA */
#define DBT_IRES_DEFAULT_UA 12500u            /* FLASH_LED_IRES_DEFAULT_UA == [D] torch_0 qcom,ires-ua */
#define DBT_IRES_DEFAULT_IDX 0u               /* FLASH_LED_IRES_DEFAULT_VAL */
#define DBT_IRES_LEVELS     4u
#define DBT_IRES_MAX_MA     {1600, 1280, 960, 640}   /* max_ires_curr_ma_table ([V]:369-372) */
/* PM8150L_IRES_WA: [V]:2896 sets it for this PMIC and [V]:1367 makes a programmed current of at most
 * 20 mA step on 5 mA instead of 12.5 mA. It is a hardware erratum workaround, so it is not optional. */
#define DBT_WA_MAX_MA       20u

/* ---- this unit's torch node ([D] qcom,torch_0 + qcom,led_switch_0) ---- */
#define DBT_TORCH_ID        0u                /* qcom,id = <0> */
#define DBT_LED_MASK        0x1u              /* led_switch_0 qcom,led-mask = <0x1>: channel 0 only */
#define DBT_TORCH_MA        300u              /* qcom,current-ma = <0x12c> */
#define DBT_TORCH_MAX_MA    500u              /* qcom,max-current = <0x1f4>: the module's own ceiling */
#define DBT_TORCH_IRES_UA   12500u            /* qcom,ires-ua = <0x30d4> */
#define DBT_TORCH_TMR       DBT_V_TMR_DISABLE /* no qcom,duration-ms on torch_0 -> [V]:2249 */
#define DBT_STROBE_CTRL_SW  0x00u             /* [V]:2093 SW_STROBE: hw_strobe/edge/active_high all 0 */
/* [D] qcom,thermal-derate-en is present and qcom,thermal-derate-current = <0xc8 0x1f4 0x3e8>, i.e.
 * 200/500/1000 mA for OTST levels 1-2/3-4/5-6. The vendor's lookup is thermal_derate_current[otst>>1]
 * with otst in 0..7, so indices 6 and 7 read one past three cells ([V]:1290-1318). Clamping to the
 * last cell is the only deviation from [V] in this file, and it is a bounds fix, not a policy. */
#define DBT_DERATE_MA       {200, 500, 1000}
#define DBT_DERATE_LAST     2u

/* ---- the port's own numbers, inherited from PaganiTorch's policy header ---- */
#define DBT_LEVEL_MAX       1000u             /* the native torch level range the OS hands down */
#define DBT_AUTO_OFF_MS     300000u           /* 300 s: the light may not stay on forever by itself */
#define DBT_POLL_MIN_MS     10u
#define DBT_POLL_MAX_MS     1000u
#define DBT_POLL_DEFAULT_MS 100u
#define DBT_CHANNELS        4u                /* the block drives 4 channels; this board uses id 0 */

#endif
