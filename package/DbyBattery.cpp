// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// DbyBattery.cpp — the battery iOS sees on DBY-W09 (SM8250 / kona), read off the PM8150B over SPMI.
//
// WHAT THIS FILE IS
//   An IOPMPowerSource whose values come from the PM8150B's gen4 fuel gauge and SMB5 charger. powerd
//   finds every IOPMPowerSource attached to the platform, reads CurrentCapacity/MaxCapacity,
//   IsCharging, ExternalConnected, Voltage, Amperage and BatteryInstalled out of the IORegistry and
//   listens for the message updateStatus() sends; the status bar, the lock-screen charging state and
//   Settings > Battery follow from that. IOPMPowerSource is already in this kernel — llvm-nm on
//   kernel.macho has __ZN15IOPMPowerSource13setPSPropertyEPK8OSSymbolP8OSObject as an external — so
//   this adds no IOKit object to the link.
//
//   The transport is the SPMI arbiter itself, not the audio DSP. The official SM8250 board this port
//   follows (upstream alioth) reads its battery in package/ShengBattery.cpp over PMIC GLINK on an ADSP
//   channel named "PMIC_RTR_ADSP_APPS". That path cannot exist on this tablet: this image has no ADSP
//   stack (AdspProbe, QcomGlink, QcomSmem, QcomSmp2p, QcomQmp and QcomRpmh are in no tree here), no
//   *.mbn ADSP firmware anywhere on the build machine or in the device's own images, and the board's
//   firmware never mentions a PMIC RTR — the stock DTBO for this unit has zero hits for `glink`, `rtr`
//   or `pmic_rr`. What that DTBO does carry, at b01.dts:574-620, is `qpnp,fg { compatible =
//   "qcom,fg-gen4"; }` with blocks at 0x4000/0x4100/0x4200/0x4300, and `qcom,qpnp-smb5` at
//   b01.dts:447-470: the gauge and the charger as plain SPMI peripherals of USID 2 (vendor
//   pm8150b.dtsi:11-13). So this file keeps the official class, the official stats ABI and the official
//   property set, and changes only the bus underneath them.
//
// READ-ONLY BY CONSTRUCTION
//   Every 32-bit word this file writes to hardware is an observer command word carrying a READ opcode
//   (kOpForms). It never maps the arbiter's write-channel window (`chnls` 0x0c600000 + 0x2000000,
//   which neither this file nor package/DbyTouch/DbyTouch.cpp maps), never writes a PMIC configuration
//   register, reads the charger's enable bit without ever writing it, and never touches SMB5's
//   bark/bite watchdog. Whether the cell is being charged, and how hard, stays the firmware's
//   decision; nothing here can start, stop or bias it.
//
// NO NUMBER IS INVENTED
//   A field that did not answer is absent from the published dictionary and -1 in
//   native_battery_stats[], never 0. IOPMPowerSource::init() creates only the OSSymbols: the properties
//   dictionary starts empty and updateStatus() publishes exactly the keys that were set, so omitting a
//   setter is how "unknown" is said, and it is what makes this rule implementable. One key the official
//   driver sets is published as upstream's own unknown instead of dropped: TimeRemaining is set to
//   0xffff, exactly what ShengBattery.cpp:105-106 does when the gauge gave no minutes. The rest stay
//   out because reading them here would mean writing to the PMIC. Temperature, DesignCapacity and the
//   full/raw mAh pair all live in the gen4 gauge's interleaved SRAM behind fg_sram_read()
//   (qpnp-fg-gen4.c:874-878 and :913-917), and AdapterDetails because the charger facts readable
//   without a write are USBIN_PLUGIN, POWER_PATH_STATUS, USBIN_CMD_IL and the ICL status bytes - a
//   cable, whether it feeds the board, whether its input is suspended and how hard it may pull - but
//   not a family code, watts or current; an empty adapter dictionary is worse than none. The health
//   bits the status rule needs are plain registers and are read (smb5-reg.h:56/:57).
//
// WHAT DECIDES "CHARGING"
//   Not this file. It is the compiled branch of smblib_get_prop_batt_status() in this tablet's own
//   kernel - Huawei's, because arch/arm64/configs/merge_kona_defconfig:5912 sets
//   CONFIG_HUAWEI_POWER_EMBEDDED_ISOLATION=y - which reads usb_present, input_suspend, the charge-enable
//   votable and the battery health, and does not look at the SMB5 3-bit regulation stage at all
//   (drivers/power/supply/qcom/smb5-lib.c:2111-2162). ExternalConnected is that file's usb_online
//   (:3556-3560), FullyCharged its charge_done (:2549) gated on being plugged, with upstream's
//   ShengBattmgr.c:158 percent==100 term.
//
// BOOT ARGUMENT
//   `dby-battery=1` starts the service (package/BatteryProbe.cpp) read-only; `dby-battery=2` also
//   lets it write the USBIN input-current registers: it measures what the cable gives before it
//   arms anything, arms only a value from the vendor's own table, and puts HW_AUTO_MODE back if the
//   hardware says the source refused. Absent, or 0, that file returns
//   before mapping anything and the image behaves exactly like today's. The name is this board's dby-
//   convention (dby-bl, dby-pa) rather than the handover's `sheng-battery`, measured against the
//   1000-byte command-line cap in core/scripts/unixnu/payload.py:35 — see board.toml's comment on the
//   argument for the byte count.
//
// WHERE THE RESULT IS VISIBLE
//   native_battery_stats[16], the official ABI, is what powerd and the IORegistry consumers read.
//   The array itself has no printer in this tree, and the comment that said it did was pointing at a
//   file that is not in the kernel: upstream's would-be reader package/BootLog.c:108 - :837-843
//   renders the `[battery]` row - is in no archive this board builds. Measured 2026-11-06: llvm-ar t
//   on build/xnu/.../libBootProgress.a lists five objects, none of them BootLog.o, and
//   scripts/build-boot-progress.py never names BootLog.c at all.
//   What is visible is this file's own rows, through kprintf. The index meanings are upstream's
//   (ShengBattery.cpp:34-37), re-pointed at this transport and listed where they are written.
//   Why kprintf and not IOLog is a measurement, not a preference: the 2026-11-06 USB capture carried
//   418 lines, every [touch] and [pa] kprintf row in it, and not one IOLog-family row anywhere - this
//   board's COM4 mirrors kprintf only. Census and per-sample detail therefore goes to kprintf, on the
//   console this board has carried since the 2026-11-05 round (board.toml:221 `serial=3`; the host
//   reader must set DtrEnable/RtsEnable).
//
// UNVERIFIED — read this before believing any of the above
//   * No battery row has reached a console yet. The 2026-11-06 boot did carry `dby-battery=1` and
//     printed nothing battery-shaped, which the sink finding above explains - every line this file
//     wrote went to IOLog, and IOLog does not reach COM4 - so that boot answers nothing about the
//     PMIC in either direction. The neighbouring path has run: package/DbyTouch/DbyTouch.cpp
//     dbyPmicUp/dbyPmicRead drives this exact observer engine, on this exact tablet, from XNU, for the
//     PON and GPIO6 reads. What has never been read on this board is the arbiter's core/cnfg table
//     windows, or any channel of a fuel-gauge PPID.
//   * The APIDs are whatever firmware left in the map at hand-off, and `qcom,ee = <0>` (kona.dtsi:3492)
//     is the DT's claim about this EE, not a measurement. The ownership table is what makes a mismatch
//     visible, which is why census() reports it rather than trusting it.
//   * The command-word spelling is derived, not guessed — see kOpForms. Whether firmware leaves this
//     EE's observer channels usable at this boot stage is what the first boot answers; the ladder costs
//     at most three timeouts on one byte, ends that cycle, and is walked again by the next one.
//   * Every decode constant is quoted from the vendor driver for this PMIC and has not been compared
//     against Android's reading of the same registers on this unit. The range gates are the guard
//     against that being silently wrong.
//
// Sibling references, all in the same force-loaded archive: the volatile 32-bit MMIO read is
// package/CpuFreqProbe.cpp:53; the window mapping is CpuFreqProbe.cpp:78-84 (IODeviceMemory::withRange
// then map(kIOMapAnywhere | kIOMapInhibitCache), kept for the life of the boot); the entry point and
// the class-registration sequence are package/BatteryProbe.cpp, copied from upstream's
// package/BatteryProbe.cpp.

#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <IOKit/pwr_mgt/IOPMPrivate.h>   // kIOPMSetValue (:156), kIOPMSetACAdaptorConnected (:160)
#include <IOKit/pwr_mgt/RootDomain.h>    // IOPMrootDomain::receivePowerNotification (:266)
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>

// The official ABI. Declared, not defined: package/BootProgressStubs.c:209 holds the one weak
// definition this tree gives every native_* stats array, package/BootLog.c:108 reads it as int32_t[16],
// and AppleARMBacklight.cpp:54 and upstream's ShengBattery.cpp:34 are extern-only for the same reason.
extern "C" {extern volatile int32_t native_battery_stats[16];}

namespace {

// ---- the arbiter, from the vendor DT ---------------------------------------------------------------
//
// PM8150B (USID 2) is reachable only through the SPMI PMIC arbiter. The windows are verbatim from
// arch/arm64/boot/dts/vendor/qcom/kona.dtsi:3523-3529: "core" 0x0c440000 +0x1100,
// "chnls" 0x0c600000 +0x2000000, "obsrvr" 0x0e600000 +0x100000, "intr" 0x0e700000 +0xa0000,
// "cnfg" 0x0c40a000 +0x26000. `chnls`, the request-side engine, stays unmapped while this service
// only reads: with dby-battery=1 every word this file sends carries a read opcode. Under
// dby-battery=2 it maps at most TWO channels of it - the USBIN input block and the MISC block, the
// only two peripherals the vendor's own input-current path writes - and each only after the
// ownership table says this EE holds that channel's write path.
constexpr uint64_t kCoreBase = 0x0C440000ull, kCoreSize = 0x1100ull;   // kona.dtsi:3484
constexpr uint64_t kCnfgBase = 0x0C40A000ull;
// The DT declares 0x26000 for cnfg (kona.dtsi:3488); the ownership table read here ends at
// 0x700 + 4*512 = 0xEFC, so one page is the whole requirement and mapping less than the DT declares is
// intentional.
constexpr uint64_t kCnfgSize = 0x1000ull;
constexpr uint64_t kObsBase = 0x0E600000ull, kObsSize = 0x100000ull;   // kona.dtsi:3527 "obsrvr"

// The request side, from the same node: "chnls" is kona.dtsi:3526 and spmi-pmic-arb.c binds it as
// wr_base (:1234-1236). A write is pmic_arb_write_cmd() (:391-446) and its order is not negotiable:
// the channel offset is 0x10000 * apid for PMIC_ARB_CHANNEL_RW (pmic_arb_offset_v5, :1013) after the
// write_ee check at :1007-1011 returns -EPERM for any other EE; the data byte goes to WDATA0 first,
// packed little-endian into a u32 by pmic_arb_write_data() (:232-239); only then the command word at
// +0x00; and the DONE poll runs on that same request channel (:243-275). CMD/STATUS/RDATA keep the
// offsets the observer side already uses (:26-32), WDATA0 is 0x10.
constexpr uint64_t kChnlsBase = 0x0C600000ull;
constexpr uint64_t kChnlsRegionSize = 0x2000000ull;   // the chnls window size the dts gives
constexpr uint32_t kChnlsSize = 0x100ull;      // one channel: CMD 0x00 through WDATA1 0x14
constexpr uint64_t kRwStride = 0x10000ull;     // 0x10000 * apid (spmi-pmic-arb.c:1013)
constexpr uint32_t kRegWdata0 = 0x10u;         // PMIC_ARB_WDATA0 (:29)

// The v5 APID -> PPID map and the per-APID write-EE table (all drivers/spmi/spmi-pmic-arb.c):
//   0x900 + 4*n  pmic_arb_apid_map_offset_v5, :1113-1116
//   0x700 + 4*n  SPMI_OWNERSHIP_TABLE_REG,    :50
constexpr uint32_t kApidMapBase = 0x900u, kOwnershipBase = 0x700u;
constexpr uint32_t kRegVersion = 0x0000u;          // PMIC_ARB_VERSION, :20
constexpr uint32_t kVersionV5Min = 0x50000000u;    // PMIC_ARB_VERSION_V5_MIN, :23
constexpr uint32_t kVersionMaxExclusive = 0x70000000u;
constexpr uint32_t kPpidMask = 0xFFFu;             // PMIC_ARB_PPID_MASK, :97
constexpr uint32_t kPpidShift = 8u;                // ppid = (regval >> 8) & PMIC_ARB_PPID_MASK, :917
constexpr uint32_t kApidIrqOwnerBit = 1u << 24;    // PMIC_ARB_CHAN_IS_IRQ_OWNER, :46 / :918
constexpr uint32_t kWriteEeMask = 0x7u;            // SPMI_OWNERSHIP_PERIPH2OWNER, :51
constexpr unsigned kMaxApids = 512u;               // PMIC_ARB_MAX_PERIPHS, :92

// Observer-channel geometry: 0x10000 * ee + 0x80 * apid (spmi-pmic-arb.c:1000-1004) with
// CMD/STATUS/RDATA0 at 0x00/0x08/0x18 (:27, :29, :32); STATUS DONE is bit 0 and
// FAILURE|DENIED|DROPPED are bits 1..3 (:55-58).
constexpr uint32_t kObsEeStride = 0x10000u, kObsApidStride = 0x80u;
constexpr uint32_t kRegCmd = 0x00u, kRegStatus = 0x08u, kRegRdata0 = 0x18u;
constexpr uint32_t kStatusDone = 1u << 0, kStatusErrorMask = 0xeu;
// The vendor's own wait is "u32 timeout = PMIC_ARB_TIMEOUT_US" (:93, which is 1000) decremented once
// per readl_relaxed(), with udelay(1) between the reads (:247-283): one millisecond of polling per
// transaction. The 200000 free-running reads this file used instead are not in that shape, and on a
// silent engine they held this service's timer thread - which is the default work loop, shared with
// every other driver - for hundreds of milliseconds per cycle.
constexpr unsigned kPolls = 1000u;

// The EE the DT says the AP is (kona.dtsi:3492 `qcom,ee = <0>`), used only to prefer this EE's own
// APID when a PPID is mapped more than once — pmic_arb_read_apid_map_v5's "First PPID mapping or one
// for this EE" (spmi-pmic-arb.c:930-932).
constexpr unsigned kOwnEe = 0u;

// ---- the peripherals this service reads ------------------------------------------------------------
//
// PPID = (USID << 8) | (peripheral address >> 8) — spmi-pmic-arb.c:977 (v2) and :996 (v5). USID 2 and
// the block addresses are vendor pm8150b.dtsi: :11-13 (`qcom,pm8150b@2`), :97-98 (charger 0x1000),
// :138 (0x1200), :157-158 (usb 0x1300), :310-312 (fg-batt-soc 0x4000), :336 (batt-info 0x4100).
constexpr uint32_t kPmicUsid = 2u;
constexpr uint32_t kPpidChgr = (kPmicUsid << 8) | 0x10u;      // 0x210 SMB5 charger
constexpr uint32_t kPpidPdet = (kPmicUsid << 8) | 0x12u;      // 0x212 monitor / presence block
constexpr uint32_t kPpidUsb = (kPmicUsid << 8) | 0x13u;       // 0x213 USBIN
constexpr uint32_t kPpidDcdc = (kPmicUsid << 8) | 0x11u;      // 0x211 DCDC (smb5-reg.h:12)
constexpr uint32_t kPpidFgSoc = (kPmicUsid << 8) | 0x40u;     // 0x240 fg-batt-soc
constexpr uint32_t kPpidFgInfo = (kPmicUsid << 8) | 0x41u;    // 0x241 fg-batt-info
// AICL_CMD_REG is MISC_BASE 0x1600 + 0x44 (smb5-reg.h:14 and :528), so the peripheral that carries
// it is USID 2's 0x16. It is the second - and last - peripheral this file is allowed to write.
constexpr uint32_t kPpidMisc = (kPmicUsid << 8) | 0x16u;      // 0x216 MISC (AICL_CMD_REG lives here)
// TYPEC_BASE is 0x1500 (smb5-reg.h:16), so the sink-status byte is USID 2's 0x15 + 0x06
// (TYPE_C_SNK_STATUS_REG, smb5-reg.h:352).  It is read, never written: the first thing
// smblib_set_icl_current() does is return without configuring a limit when that byte says a
// debug-accessory sink (smb5-lib.c:1498-1500), so no software ICL is legal until it is known.
constexpr uint32_t kPpidTypec = (kPmicUsid << 8) | 0x15u;     // 0x215 TYPEC (read only)
constexpr uint8_t kAdTypecSnkStatus = 0x06u;
constexpr uint8_t kSnkDamMask = 0x70u;  // SNK_DAM_MASK GENMASK(6,4), smb5-reg.h:354

// Register addresses inside those blocks, each the vendor macro with its block base folded in.
// MSOC and its copy: fg-util.c:859-910, where the vendor refuses a disagreeing pair. VBAT and VBAT_CP:
// fg-reg.h:241-242 and :254-255, converted at fg-util.c:1015-1054. IBAT and IBAT_CP: fg-reg.h:244-246
// and :256-258, converted at fg-util.c:960-1013. BATTERY_CHARGER_STATUS_1 bits 2:0: smb5-reg.h:30-41.
// CHARGING_ENABLE_CMD bit 0, read only: smb5-reg.h:66. INT_RT_STS is 0x10 inside every block
// (fg-reg.h:183-188 names the same offset for the gauge); USBIN_PLUGIN_RT_STS_BIT is BIT(4)
// (smb5-reg.h:240) and the 0x1200 block's bits 5:4 are BAT_TERMINAL_MISSING and
// BAT_THERM_OR_ID_MISSING, the two bits pm8150b.dtsi:151 names and fg-util.c checks before trusting a
// gauge at all.
constexpr uint8_t kAdSoc = 0x09u, kAdSocCp = 0x0Au;
constexpr uint8_t kAdVbatt0 = 0xA0u, kAdVbatt1 = 0xA1u;
constexpr uint8_t kAdVbattCp0 = 0xA6u, kAdVbattCp1 = 0xA7u;
constexpr uint8_t kAdIbat0 = 0xA2u, kAdIbat1 = 0xA3u;
constexpr uint8_t kAdIbatCp0 = 0xA8u, kAdIbatCp1 = 0xA9u;
constexpr uint8_t kAdChgSts1 = 0x06u, kAdChgEn = 0x42u, kAdIntRtSts = 0x10u;
constexpr unsigned kUsbinPlugBit = 4u, kBatMissingShift = 4u;

// The registers the vendor's own charge decision is read from.
//   POWER_PATH_STATUS = DCDC_BASE + 0x0b (smb5-reg.h:141): USE_USBIN bit 4 and
//   VALID_INPUT_POWER_SOURCE_STS bit 0 together are smblib_get_prop_usb_online()'s answer
//   (smb5-lib.c:3556-3560). USE_USBIN bit 4 is an enable mirror the host writes through the
//   usbin_path_enable votable, so on a board where no host driver ever writes it that bit cannot be
//   the term that says a cable has left; USBIN_PLUGIN_RT_STS bit 4 is the vendor's real-time answer.
//   USBIN_CMD_IL = USBIN_BASE + 0x40 (smb5-reg.h:244) bit 0 is USBIN_SUSPEND_BIT (:245), the only bit
//   the vendor touches there: smblib_get_usb_suspend() reads it (smb5-lib.c:418-431) and
//   smblib_set_usb_suspend() is its only writer (:770-790), which also votes icl_irq_disable while
//   suspended. The status rule at :2137 subtracts chg->input_suspend, the driver's cache of exactly
//   that bit; with no charger driver in this boot the register is the only surviving copy of whoever
//   voted last. POWER_PATH_STATUS bit 6 is the echo of the vote and stays in the row as sus=.
//   BATTERY_CHARGER_STATUS_7 = CHGR_BASE + 0x0d (smb5-reg.h:51): TOO_COLD bit 2 and TOO_HOT bit 3
//   (:56/:57) are the two health states smblib_get_prop_batt_health() reports without needing anything
//   but the byte, and smblib_get_prop_batt_status() turns either of them into NOT_CHARGING
//   (smb5-lib.c:2155-2161).
constexpr uint8_t kAdPowerPath = 0x0Bu, kAdChgSts7 = 0x0Du;
constexpr unsigned kUsbinSuspendStsBit = 6u, kUseUsbinBit = 4u, kValidInputStsBit = 0u;
constexpr unsigned kTempTooColdBit = 2u, kTempTooHotBit = 3u;

// The five bytes that between them name every official reason a cell would not take current. Each
// offset is a vendor macro with its block base folded in: USBIN_CMD_IL_REG smb5-reg.h:244,
// USBIN_CURRENT_LIMIT_CFG_REG :317, ICL_MAX_STATUS_REG :133, AICL_ICL_STATUS_REG :135,
// CHGR_FLOAT_VOLTAGE_CFG_REG :89. Only bit 0 of the first is interpreted (USBIN_SUSPEND_BIT :245)
// because nothing in this kernel's driver tree decodes its other seven bits; it prints as the byte.
// The three numbers use smblib_get_charge_param()'s own arithmetic, raw * step_u + min_u
// (smb5-lib.c:396-416), against the table this board is bound to: smb5_pm8150b_params
// (qpnp-smb5.c:128-165), selected by PMIC subtype at :349-356 - fv min 3600000 step 10000 uA,
// usb_icl and both icl statuses min 0 step 50000 uA. That table, not the PMI632 one at :45, is the
// one this tablet's driver would have used, which is why the applied limit reads 0x08 rather than
// the ICL_STATUS_REG 0x07 the other variant names.
constexpr uint8_t kAdUsbinCmdIl = 0x40u, kAdUsbinIclCfg = 0x70u;
constexpr uint8_t kAdIclMax = 0x06u, kAdIclStatus = 0x08u, kAdFloatVolt = 0x70u;
constexpr unsigned kInputSuspendBit = 0u, kIclStepMa = 50u;
constexpr unsigned kFloatBaseMv = 3600u, kFloatStepMv = 10u;

// The registers the input-current path writes, each a vendor macro with its block base folded in
// (smb5-reg.h:11-17 names USBIN_BASE 0x1300 and MISC_BASE 0x1600):
//   APSD_STATUS_REG :207, APSD_RESULT_STATUS_REG :217, USBIN_CMD_IL_REG :244, CMD_APSD_REG :247,
//   CMD_ICL_OVERRIDE_REG :250, USBIN_LOAD_CFG_REG :310, USBIN_ICL_OPTIONS_REG :312,
//   USBIN_CURRENT_LIMIT_CFG_REG :317, AICL_CMD_REG :528.
// Their bits: APSD_DTC_STATUS_DONE_BIT :214, QC_CHARGER_BIT :213, APSD_RESULT_STATUS_MASK
// GENMASK(6,0) :219, the seven result bits :220-226, USBIN_SUSPEND_BIT :245, ICL_OVERRIDE_BIT
// :251, ICL_OVERRIDE_AFTER_APSD_BIT :311, CFG_USB3P0_SEL/USB51_MODE/USBIN_MODE_CHG :313-315,
// RERUN_AICL_BIT :530 and the eight USBIN INT_RT_STS bits :235-242 (ICL_CHANGE 7,
// SOURCE_CHANGE 6, REVI 5, PLUGIN 4, OV 3, UV 2, VASHDN 1, COLLAPSE 0).
constexpr uint8_t kAdApsdStatus = 0x07u, kAdApsdResult = 0x08u;
// CMD_APSD_REG (USBIN_BASE + 0x41, APSD_RERUN_BIT smb5-reg.h:247-248) is deliberately absent
// from this table: kona/DBY/common/huawei_charger.dtsi:147 sets usb-thd-bc-12, so
// smblib_apsd_enable() clears BC1P2_SRC_DETECT_BIT on purpose (smb5-lib.c:1005-1010) and the
// adapter type arrives from the FSA9685 instead (qpnp-smb5.c:3390-3414).  Re-running PMIC
// APS-D would race that.  The only override register we touch is CMD_ICL_OVERRIDE_REG.
constexpr uint8_t kAdCmdIclOverride = 0x42u;
constexpr uint8_t kAdUsbinLoadCfg = 0x65u, kAdUsbinIclOptions = 0x66u;
constexpr uint8_t kAdAiclCmd = 0x44u;
constexpr unsigned kApsdDoneBit = 0u, kQcChargerBit = 1u;
constexpr uint8_t kApsdResultMask = 0x7Fu;
constexpr uint8_t kSdpResultBit = 0x01u, kOcpResultBit = 0x02u, kCdpResultBit = 0x04u,
                kDcpResultBit = 0x08u, kFloatResultBit = 0x10u, kQc2ResultBit = 0x20u,
                kQc3ResultBit = 0x40u;
constexpr unsigned kUsbinSuspendSetBit = 0u, kCmdIclOverrideBit = 0u,
                   kLoadCfgOverrideAfterApsdBit = 4u, kAiclRerunBit = 0u;
constexpr unsigned kIclOptionsUsb3p0Bit = 2u, kIclOptionsUsb51Bit = 1u,
                   kIclOptionsModeChgBit = 0u;
// Only two of the eight USBIN INT_RT_STS bits are used as a verdict: COLLAPSE, whose hardware
// response the driver itself arms (SUSPEND_ON_COLLAPSE_USBIN_BIT, qpnp-smb5.c:3165/:3168), and
// UV, the VBUS sag that an over-draw produces.  OV, VASHDN, PLUGIN, REVI, SOURCE_CHANGE and
// ICL_CHANGE (smb5-reg.h:235-242) are not criteria for stepping the limit, so they are not
// given names here.
constexpr unsigned kUsbinCollapseBit = 0u, kUsbinUvBit = 2u;

// The four input limits this board's own build asks a USB source for, and the only four this
// service is allowed to arm: the vote is picked out of this table by the measured ceiling, so
// an arm above what the source was measured giving is not reachable from any code path. They are the values
// update_sw_icl_max() votes per type (smb5-lib.c:6243-6275) out of the table merge_kona_defconfig:5912
// compiles - CONFIG_HUAWEI_POWER_EMBEDDED_ISOLATION=y picks the Huawei branch of smb5-lib.h:104-119,
// where SDP is 500000, FLOAT 1200000, CDP 1500000 and DCP 2300000 uA.
//
// Two numbers are deliberately absent. 900 mA is not one of them because the only way the vendor
// reaches it is Type-C Rp medium/high or a SuperSpeed enumeration (smb5-lib.c:6224-6233 and
// :6246-6248's own comment), this board's DWC3 is pinned DWC3_DCFG_HIGHSPEED
// (package/QcomXnuUsbDebug/usb/Dwc3.c:533-534, validated at :415-419) and the DT sets neither Rp
// path - so on this tablet's USB-2 host port 500 mA is the vendor's ceiling and asking above it is
// over-draw, not a fix. HVDCP's 3000000 is absent because that value is only legal after reglit has
// asked the adapter for 9 V or 12 V, and nothing in this boot drives that negotiation; writing 60
// into 0x1370 would ask a 5 V source for 3 A.
struct IclStep { const char *name; int ua; };
constexpr IclStep kIclLadder[] = {
    { "sdp",   500000 },   // SDP_CURRENT_UA, smb5-lib.h:106
    { "float", 1200000 },  // FLOAT_CURRENT_UA, :110
    { "cdp",   1500000 },  // CDP_CURRENT_UA, :107
    { "dcp",   2300000 },  // DCP_CURRENT_UA, :109 (the Huawei-isolation value)
};
constexpr unsigned kIclStepCount = sizeof kIclLadder / sizeof kIclLadder[0];   // 4 steps
// How many sample tiers to leave between the arm and the verdict.  The vendor's own window is
// ICL_CHANGE_DELAY_MS = 1000 ms (smb5-lib.c:5678, used by icl_change_irq_handler at :5682-5723 to
// re-schedule icl_change_work after an ICL change).  One tier is 2 s, so a single tick already
// exceeds it; anything more would just be slower to report.
constexpr unsigned kIclSettleTicks = 1u;

// apsdName() below answers the same question that smblib_get_apsd_result() does
// (smb5-lib.c:594-608): the masked result byte is compared against `.bit` exactly, OCP is its
// own type, and QC_CHARGER_BIT in APSD_STATUS forces an HVDCP name unless the byte already
// says HVDCP3.  No table is needed for that, only the bit constants above.

// The vendor's own conversions: 122070 uV per LSB and 488281 uA per LSB (fg-reg.h:110/:113, applied at
// fg-util.c:1015-1054 and :960-1013). The products do not fit 32 bits — 65535 * 122070 is 8.0e9 — so
// they are computed at 64. MSOC is not linear: 255 is full, 0 is empty, and 1..254 map onto 1..99 as
// DIV_ROUND_CLOSEST((raw-1)*(100-2), 253)+1 (fg-util.c:859-910).
constexpr uint64_t kVoltNum = 122070ull, kVoltDen = 1000000ull;
constexpr int64_t kCurrNum = 488281, kCurrDen = 1000000;
constexpr uint32_t kMsocSlope = 98u, kMsocSpan = 253u;
// The physically impossible is how a wrong decode announces itself.
constexpr int kVoltMinMv = 2500, kVoltMaxMv = 5000, kCurrMaxMa = 20000;

// The SMB5 charge stage, BATTERY_CHARGER_STATUS_1 bits 2:0 (smb5-reg.h:30-41).
enum Stage : int { kStageInhibit = 0, kStageTrickle = 1, kStagePre = 2, kStageFullOn = 3,
                   kStageTaper = 4, kStageTerminate = 5, kStagePause = 6 };

// Three spellings of the command word, all READ opcodes, in the order the evidence puts them.
//
// 1 first, and it is not a guess but what this board's own drivers send. The gauge and the charger are
//   regmap clients with 2-byte register addresses (regmap-spmi.c:118 BUG_ON(reg_size != 2)) and every
//   address in this read set is above 0xFF, so regmap_spmi_read() splits them onto
//   spmi_ext_register_readl() (:133/:145); spmi.c:189 turns that into SPMI_CMD_EXT_READL = 0x38 and the
//   arbiter maps 0x38..0x3F onto PMIC_ARB_OP_EXT_READL = 1 (spmi-pmic-arb.c:67, :361-363).
//   DbyTouch.cpp issues exactly opcode 1 on exactly this tablet (kPmicOpRead = 1 at :4640, used at
//   :4740-4741) — the only hardware answer any SPMI command on this board has.
// 15 second: PMIC_ARB_OP_READ, the direct read the same switch reaches at opc 0x60..0x7F, for the case
//   where a v5 arbiter takes the SID from the APID map and the channel already *is* the peripheral.
// 13 with the SID at bits 23:20 last, and knowingly *not* what this driver tree does on v5:
//   pmic_arb_fmt_cmd_v2 (spmi-pmic-arb.c:1025) drops the SID and only fmt_cmd_v1 (:1020) writes it. It
//   stays because this board has already shown source and hardware disagreeing about a layout — the v7
//   assumption that answered rc=2 "spmi:layout" on 2026-11-01 — and it is third so that the insurance
//   costs a timeout only once both source-derived spellings have failed.
struct OpForm { uint32_t opcode; bool withSid; };
constexpr OpForm kOpForms[] = { {1, false}, {15, false}, {13, true} };
constexpr unsigned kOpFormCount = 3u;
// The same three shapes on the write side, with the opcodes the same file's write switch produces:
// spmi_ext_register_writel() sends SPMI_CMD_EXT_WRITEL + 0 = 0x30 (spmi.c:267, include/linux/spmi.h:35)
// for any register above 0xFF because regmap-spmi.c:159-178 splits those onto the long form, and
// pmic_arb_write_cmd() maps 0x30..0x37 onto PMIC_ARB_OP_EXT_WRITEL = 0 (:415-417, :65-70). 0x40..0x5F
// would be PMIC_ARB_OP_WRITE = 14 and 0x00..0x0F PMIC_ARB_OP_EXT_WRITE = 2, so those are the second
// and third spellings; the SID rule is the read ladder's rule (fmt_cmd_v2 drops it, fmt_cmd_v1 keeps
// it - :1020-1028).
constexpr OpForm kWriteForms[] = { {0, false}, {14, false}, {2, true} };
constexpr unsigned kWriteFormCount = 3u;
// Every register the write path addresses must sit inside the one 256-byte channel that was
// mapped for it.  This is the check that would have failed the 2026-11-06 data abort at
// compile time instead of on the tablet.
static_assert(kRegWdata0 + 4u <= kChnlsSize, "write register outside the mapped channel");
constexpr uint32_t kCmdOpShift = 27u, kCmdAddrShift = 4u, kCmdSidShift = 20u;

constexpr unsigned kApidTable = 0x60u;   // the USID-2 peripheral ids in use run 0x10..0x41

// Two timers, which is upstream's own shape: ShengBattery.cpp keeps a 60 s refresh timer for the whole
// cycle and a 2 s kRequestMs beside it (ShengBattery.cpp:47 kRequestMs = 2000, kRefreshMs = 60000). The
// fast tier here re-reads only the fields the charge rule consumes and publishes them; the 60 s tier
// keeps the full official cycle. With one 60 s timer a plug or an unplug waited up to a whole cycle to
// reach powerd, which is the measured 「反应很迟钝，得等好一会才正确反馈」. The vendor never waits that
// long because its driver reacts on the usbin_change_pend interrupt; this service has no PMIC interrupt
// path at all (native_battery_stats[9] is 0 and bit 4 of [5] says notifications are not enabled), so the
// 2 s tier is the substitute until an interrupt is wired.
constexpr uint32_t kFirstMs = 250, kRefreshMs = 60000, kStatusMs = 2000;

// ---- one refresh cycle ---------------------------------------------------------------
//
// The shape is upstream's (package/ShengBattmgr.c): a cycle walks a field table, reads each field once,
// treats a field that does not answer as INVALID FOR THIS CYCLE and reads it again in the next one, and
// the snapshot reaches powerd only at the end of a whole cycle (ShengBattery.cpp:150,
// `if (result == BmCycleDone) publish()`). Nothing in that design can freeze a value in place; the
// closest upstream comes to giving up is sheng_battmgr_timeout(), which clears the bit of the one field
// it was holding and moves the cursor on (ShengBattmgr.c:99-106).
//
// A field is -1 unless the bytes answered, agreed with their shadow copy and decoded inside the
// physically possible range. `why` is one byte per field, in the table order below, so a field that is
// absent always has a non-zero reason beside it and an absent measurement never looks like a zero.
enum Why : uint32_t {
    kWhyOk = 0,        // the field holds a reading
    kWhyNoApid = 1,    // the APID map has no channel for that peripheral
    kWhyTimeout = 2,   // the transaction never signalled DONE
    kWhyShadow = 3,    // the register and its copy disagree; the vendor refuses these too
    kWhyRange = 4,     // the bytes decoded to a physically impossible number
    kWhyError = 5,     // DONE with FAILURE|DENIED|DROPPED set
    kWhyNoMap = 6,     // the observer window did not map, so no transaction can be issued at all
    kWhySlowOnly = 7,  // the field belongs to the 60 s tier; the 2 s tier left it alone this cycle
};

// In the order the vendor's status rule consumes them: the gauge numbers, then the presence and
// power-path bits that say a cable is feeding the board, then the charger's own enable, health and
// termination state, then the battery terminal and thermistor.
enum Field : unsigned { kFieldSoc = 0, kFieldV, kFieldI, kFieldPlug, kFieldOnline, kFieldSuspend,
                        kFieldInputCfg, kFieldIclCfg, kFieldIclMax, kFieldIclNow, kFieldFloatVolt,
                        kFieldEnable, kFieldHealth, kFieldStage, kFieldTerminal, kFieldTherm,
                        kFieldCount = 16u };

struct Sample {
    int soc, mv, ma, plug, online, suspend, il, iclc, iclm, icl, flt, enable, stage, cold, hot,
        terminalMissing, thermMissing, rtRaw;
    uint8_t why[kFieldCount];
    // The engine signalled neither DONE nor an error bit for any candidate spelling of one field. That
    // ends this cycle; it does not end the battery.
    bool engineSilent;
    unsigned txns, clean, timeouts, errors;
    // What the input current controller saw and asked, and what its own transactions cost. The RW
    // channel is a different channel from the observer one, so a request that does not answer is
    // counted here and never joins the read totals - a dead write path must not be allowed to make a
    // healthy read cycle look deaf, or the other way round.
    int apsd, apsr, want;
    unsigned wOps, wOk, wTimeouts, wErrors, wDenied;
};

inline void noteWhy(Sample &s, unsigned field, uint32_t reason) {
    s.why[field] = static_cast<uint8_t>(reason);
}

inline bool has(const Sample &s, unsigned field) {
    return s.why[field] == kWhyOk;
}

inline uint32_t rd32(uintptr_t base, uint32_t reg) {
    return *reinterpret_cast<volatile uint32_t *>(base + reg);
}

// Map one physical window uncached. The idiom and the lifetime policy are CpuFreqProbe.cpp:78-84: the
// IODeviceMemory is dropped as soon as the map exists and the map lives for the rest of the boot,
// because this service is never torn down — the platform's attachment keeps it, and the timer it owns
// has no other way to reach the arbiter.
volatile uint32_t *mapWindow(uint64_t base, uint64_t size, IOMemoryMap **keep) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    if (!memory) return nullptr;
    *keep = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (!*keep) return nullptr;
    return reinterpret_cast<volatile uint32_t *>(static_cast<uintptr_t>((*keep)->getVirtualAddress()));
}

}  // namespace

// Two IOTimerEventSources on the service's own work loop - upstream's 60 s refresh and 2 s kRequestMs
// pair, split by the field set they read rather than by a request queue. There is still no inbox:
// nothing here is asynchronous, a transaction answers inside the poll in readByte() or it is dead.
class DbyBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(DbyBattery)

    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr, *statusTimer = nullptr;
    IOMemoryMap *coreMap = nullptr, *cnfgMap = nullptr, *obsMap = nullptr;
    volatile uint32_t *obs = nullptr;
    // USID-2 peripheral id -> APID, filled by census(). -1 means firmware left no channel for that
    // block in this view, which is a per-field reason rather than a failure of the whole sample.
    int apid[kApidTable];
    // The command-word spelling: -2 until the first transaction has tried the ladder, then the index
    // that answered, or -1 once every candidate has failed. Zero-initialising this to a real index
    // would silently skip the ladder on the first read, so the -2 is load-bearing.
    int form = -2;
    bool published = false;
    unsigned samples = 0;
    // What the root domain was last told about AC: -1 has never been told. Its own field boots true
    // (IOPMrootDomain.cpp:1667) and receivePowerNotification() is the only writer (:8459-8461), so a
    // board that never calls it reports AC attached for the whole boot no matter what the registers say.
    int acTold = -1;

    // The controller's own state, which has to survive across cycles because a limit is measured in
    // one cycle, armed in the next and judged in the one after that.
    //   phase 0 idle, 1 measure, 2 read-ceiling, 3 apply, 4 settle, 5 verify, 6 hold
    //   step is the index into kIclLadder being armed, good the index last armed without a refusal,
    //   ceiling/aicl0 are the two measurements in mA, snk is TYPE_C_SNK_STATUS raw (-1 unread, never
    //   0), reran says the AICL re-run was actually issued, and locked latches the controller off for
    //   the rest of the boot.  ibPos/ibNeg count the sign of IBAT across the cycles, which is what
    //   turns 电充不进去 into two numbers instead of an interpretation.
    bool iclWrite = false;
    unsigned iclPhase = 0, iclStep = 0, iclGood = 0, iclTicks = 0;
    bool iclLocked = false, iclReran = false;
    int iclCeiling = -1, iclAicl0 = -1, iclSnk = -1;
    unsigned ibPos = 0, ibNeg = 0;
    // The command-word spelling for writes, on the same -2 rule the read ladder uses: -2 means the
    // ladder has not run, -1 means every spelling failed. Zero-initialising it would skip the ladder.
    int wForm = -2;
    bool rwTried = false;
    IOMemoryMap *rwMapUsb = nullptr, *rwMapMisc = nullptr;
    volatile uint32_t *rwUsb = nullptr, *rwMisc = nullptr;
    // Per-APID write owner, from the ownership table census() already reads: 0xff when the table did
    // not answer. spmi-pmic-arb.c will not issue a write on a channel this EE does not own (:1007-
    // 1011) and neither will this file.
    unsigned char writeEe[kApidTable];

    // ---- the bus -------------------------------------------------------------------------------

    // True only when the arbiter said DONE with no FAILURE/DENIED/DROPPED bit, which is the branch
    // structure of pmic_arb_wait_for_done() (spmi-pmic-arb.c:243-275): DONE with an error bit is a
    // failed transaction and a timeout is not a result either.
    // 1 = DONE with no error bit and data taken, 0 = never signalled DONE, -1 = DONE with an error bit.
    // The three cases are not one failure: spmi-pmic-arb.c treats them apart, and so must the caller.
    int spmiAttempt(unsigned channel, const OpForm &f, uint8_t address, uint8_t *out, Sample &s) {
        volatile uint32_t *ch = reinterpret_cast<volatile uint32_t *>(
            reinterpret_cast<uintptr_t>(obs) + kObsEeStride * kOwnEe + kObsApidStride * channel);
        uint32_t cmd = (f.opcode << kCmdOpShift) | (static_cast<uint32_t>(address) << kCmdAddrShift);
        if (f.withSid) cmd |= kPmicUsid << kCmdSidShift;
        ch[kRegCmd / 4] = cmd;                       // pmic_arb_read_cmd(), under its own spinlock
        uint32_t status = 0;
        for (unsigned poll = 0; poll < kPolls; ++poll) {
            status = ch[kRegStatus / 4];
            if (status & kStatusDone) break;
        }
        if (!(status & kStatusDone)) { ++s.timeouts; return 0; }
        if (status & kStatusErrorMask) { ++s.errors; return -1; }
        *out = static_cast<uint8_t>(ch[kRegRdata0 / 4] & 0xffu);
        return 1;
    }

    // One SPMI write, in the vendor's own order: the byte into WDATA0, then the command word, then
    // DONE on the same request channel. Only the two peripherals the input-current path touches can
    // ever reach here, and only through a channel the ownership table gave to this EE.
    // 1 = DONE clean, 0 = never signalled DONE, -1 = DONE with an error bit.
    //
    // `ch` is the channel itself.  openWriteChannels() already folded 0x10000 * apid into the
    // address it mapped, so the stride must not be applied a second time here: doing that put the
    // first WDATA0 write 0x10000 * apid past the end of a 256-byte mapping and took a kernel data
    // abort on 2026-11-06 before a single row printed.  Nothing may be addressed outside the mapped
    // size, which the static_assert by kWriteFormCount pins so this cannot come back.
    int spmiWriteAttempt(volatile uint32_t *ch, const OpForm &f,
                         uint8_t address, uint8_t value, Sample &s) {
        // bc is bits 2:0 of the command word and is len-1; one byte is therefore 0 and needs no term
        // (pmic_arb_fmt_cmd_v2, spmi-pmic-arb.c:1025).
        uint32_t cmd = (f.opcode << kCmdOpShift) | (static_cast<uint32_t>(address) << kCmdAddrShift);
        if (f.withSid) cmd |= kPmicUsid << kCmdSidShift;
        ++s.wOps;
        ch[kRegWdata0 / 4] = value;
        ch[kRegCmd / 4] = cmd;
        uint32_t status = 0;
        for (unsigned poll = 0; poll < kPolls; ++poll) {
            status = ch[kRegStatus / 4];
            if (status & kStatusDone) break;
        }
        if (!(status & kStatusDone)) { ++s.wTimeouts; return 0; }
        if (status & kStatusErrorMask) { ++s.wErrors; return -1; }
        ++s.wOk;
        return 1;
    }

    // The write-side twin of readByte(), including its ladder. Returns 0 when the byte landed; the
    // non-zero reasons are named in the row rather than smoothed over.
    int writeByte(uint32_t ppid, uint8_t address, uint8_t value, Sample &s) {
        const unsigned periph = ppid & 0xffu;
        if (!rwTried) openWriteChannels(s);
        if (periph >= kApidTable || apid[periph] < 0) { ++s.wDenied; return 1; }
        if (writeEe[periph] != kOwnEe) { ++s.wDenied; return 2; }
        volatile uint32_t *win = rwWindow(ppid);
        if (!win) { ++s.wDenied; return 3; }
        if (wForm >= 0) {
            const int r = spmiWriteAttempt(win, kWriteForms[static_cast<unsigned>(wForm)],
                                           address, value, s);
            if (r == 1) return 0;
            if (r == 0) return 4;
            wForm = -2;
        }
        for (unsigned i = 0; i < kWriteFormCount; ++i) {
            const int r = spmiWriteAttempt(win, kWriteForms[i], address, value, s);
            if (r == 1) {
                const bool changed = wForm != static_cast<int>(i);
                wForm = static_cast<int>(i);
                if (changed)
                    kprintf("DbyBattery: write opcode=%u sid=%u answered addr=0x%x\n",
                          kWriteForms[i].opcode, static_cast<unsigned>(kWriteForms[i].withSid),
                          static_cast<unsigned>(address));
                return 0;
            }
            if (r == 0) return 4;
        }
        return 5;
    }

    // regmap_update_bits() semantics, which is all smblib_masked_write() is (smb5-lib.c:86-89): read
    // the byte, keep every bit the mask does not name, write the whole byte back. A register this
    // file cannot read is a register it will not write blind.
    int maskedWrite(uint32_t ppid, uint8_t address, uint8_t mask, uint8_t value, Sample &s) {
        uint8_t cur = 0;
        const Why w = readByte(ppid, address, &cur, s);
        if (w != kWhyOk) return 100 + static_cast<int>(w);
        const uint8_t next = static_cast<uint8_t>((cur & ~mask) | (value & mask));
        return writeByte(ppid, address, next, s);
    }

    // The two windows, mapped once, here, and only under dby-battery=2. The offset arithmetic is the
    // vendor's (0x10000 * apid, spmi-pmic-arb.c:1013) and the ownership test is the -EPERM at :1007-
    // 1011 moved in front of the map: a channel another EE owns is not even asked for.
    void openWriteChannels(Sample &s) {
        rwTried = true;
        for (unsigned pass = 0; pass < 2; ++pass) {
            const uint32_t ppid = pass == 0 ? kPpidUsb : kPpidMisc;
            const unsigned periph = ppid & 0xffu;
            if (periph >= kApidTable || apid[periph] < 0) continue;
            // 512 channels of 0x10000 are exactly the chnls window the dts describes
            // (kona.dtsi:3523-3529), so an apid at or past the end names an address no mapping can
            // cover.  The value is read out of the arbiter, so it is range-checked, not trusted.
            if (static_cast<uint64_t>(static_cast<int>(apid[periph])) >= (kChnlsRegionSize / kRwStride)) {
                kprintf("DbyBattery: ppid=0x%x apid=%d is outside the chnls region: no channel "
                      "mapped\n", ppid, apid[periph]);
                continue;
            }
            if (writeEe[periph] != kOwnEe) {
                kprintf("DbyBattery: ppid=0x%x write ee=%u is not this EE's %u: no channel mapped\n",
                      ppid, static_cast<unsigned>(writeEe[periph]), kOwnEe);
                continue;
            }
            IOMemoryMap **keep = pass == 0 ? &rwMapUsb : &rwMapMisc;
            volatile uint32_t *w = mapWindow(kChnlsBase + kRwStride *
                                                    static_cast<uint64_t>(apid[periph]),
                                             kChnlsSize, keep);
            if (pass == 0) rwUsb = w; else rwMisc = w;
            kprintf("DbyBattery: rw channel ppid=0x%x apid=%d at 0x%x %s\n", ppid, apid[periph],
                  static_cast<unsigned>(kChnlsBase + kRwStride * static_cast<uint64_t>(apid[periph])),
                  w ? "mapped" : "did not map");
        }
    }

    // ---- the vendor's two input-current arms, transcribed ---------------------------------------
    //
    // smblib_set_icl_current() (smb5-lib.c:1487-1564) splits by the source type. The SDP arm
    // (:1500-1519) forces the request to SDP_CURRENT_UA under Huawei isolation and calls
    // set_sdp_current() (:1427-1481), which writes USBIN_ICL_OPTIONS' two speed bits and then
    // smblib_icl_override(SW_OVERRIDE_USB51_MODE); the tail at :1541-1559 then unsuspends the input
    // and re-runs AICL. The high-current arm writes the usb_icl charge parameter - one full byte,
    // (ua - min)/step with min 0 and step 50000 (smblib_set_charge_param :730-764, table
    // qpnp-smb5.c:143-149) - and takes SW_OVERRIDE_HC_MODE, for which the source explicitly skips the
    // AICL rerun (:1557-1559).
    //
    // Both override shapes come from smblib_icl_override() (:175-222) with this board's
    // chg->usb_third_bc_12 = true, which kona/DBY/common/huawei_charger.dtsi:147 gives by setting
    // `usb-thd-bc-12` on &pm8150b_charger and qpnp-smb5.c:472 reads. That is why SW_OVERRIDE_HC_MODE
    // here sets CMD_ICL_OVERRIDE bit 0 and clears ICL_OVERRIDE_AFTER_APSD (:189-196) instead of the
    // other way round, and why smblib_apsd_enable()'s BC1P2_SRC_DETECT_BIT is never touched: under
    // the same flag that helper forces detection OFF (smb5-lib.c:1005-1010), because this tablet asks
    // an FSA9685 rather than the PMIC what is plugged in.
    //
    // set_sdp_current()'s FORCE_FLOAT_SDP write (:1460-1473) is not reproduced: it fires only when
    // chg->real_charger_type says USB while the APS-D result says FLOAT, and real_charger_type is a
    // driver variable that the FSA9685 path fills (qpnp-smb5.c:1249). No register holds it, so an
    // offline service cannot know it and will not pretend to.
    void applySdpArm(Sample &s) {
        maskedWrite(kPpidUsb, kAdUsbinIclOptions,
                        static_cast<uint8_t>((1u << kIclOptionsUsb3p0Bit) |
                                             (1u << kIclOptionsUsb51Bit)),
                        static_cast<uint8_t>(1u << kIclOptionsUsb51Bit), s);   // :1475-1477
        maskedWrite(kPpidUsb, kAdUsbinIclOptions, 1u << kIclOptionsModeChgBit, 0, s);   // :208-212
        maskedWrite(kPpidUsb, kAdCmdIclOverride, 1u << kCmdIclOverrideBit,
                    1u << kCmdIclOverrideBit, s);                                       // :214-218
        maskedWrite(kPpidUsb, kAdUsbinLoadCfg, 1u << kLoadCfgOverrideAfterApsdBit, 0, s);  // :220-222
        maskedWrite(kPpidUsb, kAdUsbinCmdIl, 1u << kUsbinSuspendSetBit, 0, s);           // :775-777
        // smblib_run_aicl() :2683-2705: no rerun while the input is still suspended, otherwise one
        // bit in MISC 0x1644. The vendor's own tail call at :1543 repeats the three override writes
        // with the same values; one pass reaches the same register state, so the second is not sent.
        uint8_t path = 0;
        if (readByte(kPpidDcdc, kAdPowerPath, &path, s) == kWhyOk &&
            ((path >> kUsbinSuspendStsBit) & 0x01u) == 0)
            maskedWrite(kPpidMisc, kAdAiclCmd, 1u << kAiclRerunBit, 1u << kAiclRerunBit, s);
    }

    void applyHcArm(int ua, Sample &s) {
        writeByte(kPpidUsb, kAdUsbinIclCfg, static_cast<uint8_t>(ua / 50000), s);  // :754, full byte
        maskedWrite(kPpidUsb, kAdUsbinIclOptions, 1u << kIclOptionsModeChgBit,
                    1u << kIclOptionsModeChgBit, s);                               // :208-212
        maskedWrite(kPpidUsb, kAdCmdIclOverride, 1u << kCmdIclOverrideBit,
                    1u << kCmdIclOverrideBit, s);                                   // :191-196
        maskedWrite(kPpidUsb, kAdUsbinLoadCfg, 1u << kLoadCfgOverrideAfterApsdBit, 0, s);
        maskedWrite(kPpidUsb, kAdUsbinCmdIl, 1u << kUsbinSuspendSetBit, 0, s);
    }

    // HW_AUTO_MODE, the state the vendor is in when it is not overriding anything (smb5-lib.c:203-207
    // with icl_override = 0 and apsd_override = 0). This is the rollback, and it is the vendor's own
    // spelling rather than an invented "off".
    void applyHwAuto(Sample &s) {
        maskedWrite(kPpidUsb, kAdUsbinIclOptions, 1u << kIclOptionsModeChgBit,
                    1u << kIclOptionsModeChgBit, s);
        maskedWrite(kPpidUsb, kAdCmdIclOverride, 1u << kCmdIclOverrideBit, 0, s);
        maskedWrite(kPpidUsb, kAdUsbinLoadCfg, 1u << kLoadCfgOverrideAfterApsdBit, 0, s);
    }

    // The APS-D byte decoded the way smblib_get_apsd_result() decodes it, for the row and nothing
    // else: it is printed, never used to pick a limit, because under usb-thd-bc-12 the PMIC's own
    // detector is switched off and the answer in this byte is not this tablet's source of truth.
    const char *apsdName(uint8_t stat, uint8_t res) {
        if (!(stat & (1u << kApsdDoneBit))) return "not-done";
        if (res == kDcpResultBit && (stat & (1u << kQcChargerBit))) return "hvdcp2";
        if (res == (kDcpResultBit | kQc3ResultBit)) return "hvdcp3";
        if (res == (kDcpResultBit | kQc2ResultBit)) return "hvdcp2";
        switch (res) {
            case kSdpResultBit: return "sdp";
            case kOcpResultBit: return "ocp";
            case kCdpResultBit: return "cdp";
            case kDcpResultBit: return "dcp";
            case kFloatResultBit: return "float";
            default: break;
        }
        if ((stat & (1u << kQcChargerBit)) != 0) return "hvdcp2";
        return res == 0 ? "none" : "unknown";
    }

    // ---- the input-current controller ----------------------------------------------------------
    //
    // It runs on the 2 s tier because that is this tablet's own tempo for the same question:
    // bc12_change_work() retries the detection every 500 ms (qpnp-smb5.c:3390-3414) and the vendor
    // would react to the usb_plugin/apsd-done interrupts in milliseconds.
    //
    // What it does is the vendor's sequence for a plugged source, transcribed into the one state a
    // host can reach without the charger driver: check that a software limit is legal at all
    // (smb5-lib.c:1498-1500), ask the hardware what the cable gives (smblib_run_aicl(), :2683-2705),
    // read the answer back out of the two status bytes (AICL_ICL_STATUS_REG :135 and
    // ICL_MAX_STATUS_REG :133, which is what smblib_get_icl_current() reports at :1570-1580), arm
    // the largest value in this board's own table that the answer allows, and believe the
    // real-time status when it says the source refused.
    //
    // It does not climb.  The 2026-11-06 rows are why: with a free ladder the arm ended at DCP's
    // 2300 mA while the PMIC's status bytes said 500, and IBAT went from +548 mA at want=1200 to
    // -861 mA at want=2300.  An arm above what a source can give does not buy current; the input
    // sags, the charger's own voltage-controlled loop throttles to nothing, and the cell pays for
    // the system load.  The ceiling therefore picks the value and the table bounds the ceiling.
    //
    // Phases:
    //   0 idle     - no cable, or the write tier is off.  Nothing is touched.
    //   1 measure  - the DAM guard, then smblib_run_aicl(RERUN_AICL) so the status bytes are a fresh
    //                measurement of the cable that is in the port now and not Android's last latch.
    //                The input-current registers are still untouched at this point.
    //   2 ceiling  - read both status bytes; the larger is the ceiling, because a limit that some
    //                Android voter had already pulled down is not evidence that the cable cannot give
    //                more.  Neither answering is answered with SDP_CURRENT_UA, the value
    //                update_sw_icl_max() votes for an ordinary USB host port (smb5-lib.c:1516 and
    //                :6243-6248, smb5-lib.h:106).  Never 0, never "unknown".
    //   3 apply    - one arm of smblib_set_icl_current() for the voted value: the SDP/USB51 arm at
    //                500, the high-current arm above it, exactly as :1487-1564 splits them.
    //   4 settle   - kIclSettleTicks ticks without writing, so the judgement reads a settled input.
    //   5 verify   - USBIN INT_RT_STS: COLLAPSE (bit 0, whose hardware response the driver arms
    //                itself at qpnp-smb5.c:3165/:3168) or UV (bit 2) means the source refused what
    //                was just asked for, so the vote drops one table step.  A refusal at 500 - the
    //                value this board votes for a host port - ends it: HW_AUTO_MODE goes back and
    //                the controller stops for the boot, because the PMIC's own automatic limit is
    //                then the only answer left that is not mine.
    //   6 hold     - the voted value stays.  Each 60 s tier re-reads the ceiling, so a different
    //                cable re-arms; while an override is armed the status bytes echo that override,
    //                which makes the re-read stable rather than a second ladder.
    //
    // The IBAT sign tally is the outcome measure: it is the one number in this row that is not
    // produced by anything this controller wrote.
    //
    // One guard runs before any of it.  smblib_set_icl_current() refuses to configure a limit from
    // software for a debug-accessory sink (smb5-lib.c:1498-1500), and the byte that answers that
    // question is TYPE_C_SNK_STATUS_REG.  -1 means the byte never answered, which is not the same
    // statement as a DAM being present and is printed as such.
    int damSnk(Sample &s) {
        uint8_t a = 0;
        if (readByte(kPpidTypec, kAdTypecSnkStatus, &a, s) != kWhyOk) return -1;
        return static_cast<int>(a & kSnkDamMask);
    }

    // One charge-parameter status byte, decoded the way smblib_get_charge_param() decodes it:
    // raw * step_u + min_u with usb_icl min 0 step 50000 uA (smb5-lib.c:396-416 against
    // smb_pm8150b_params, qpnp-smb5.c:143-149).
    int statusMa(uint32_t ppid, uint8_t addr, Sample &s) {
        uint8_t a = 0;
        if (readByte(ppid, addr, &a, s) != kWhyOk) return -1;
        return static_cast<int>(a) * static_cast<int>(kIclStepMa);
    }

    // The last table entry at or below the ceiling.  The table is ascending, so the final match
    // wins; a ceiling under 500 answers 0 because 500 mA is what this board votes for a USB host
    // port and its table holds nothing smaller.
    unsigned voteForCeiling(int ceilingMa) {
        unsigned best = 0;
        for (unsigned i = 0; i < kIclStepCount; ++i) {
            if (kIclLadder[i].ua / 1000 <= ceilingMa) best = i;
        }
        return best;
    }

    void runIcl(Sample &s, bool full) {
        if (!iclWrite) return;
        if (s.plug < 0 || s.rtRaw < 0) return;
        if (s.plug == 0) {
            if (iclPhase != 0) {
                iclPhase = 0;
                iclReran = false;
                kprintf("DbyBattery: icl idle: input left USBIN_RT_STS at step=%u want=%d\n",
                      iclStep, kIclLadder[iclStep].ua / 1000);
            }
            return;
        }
        if (iclLocked) return;
        // Counted rather than sampled once: one IBAT reading is a moment inside a load transient,
        // the pair of counts over a boot is the answer to whether the armed limit charges the cell.
        if (s.ma > 0) ++ibPos;
        else if (s.ma < 0) ++ibNeg;

        uint8_t stat = 0, res = 0;
        Why w = readByte(kPpidUsb, kAdApsdStatus, &stat, s);
        if (w == kWhyOk) w = readByte(kPpidUsb, kAdApsdResult, &res, s);
        s.apsd = w == kWhyOk ? static_cast<int>(stat) : -1;
        s.apsr = w == kWhyOk ? static_cast<int>(res & kApsdResultMask) : -1;
        s.want = kIclLadder[iclStep].ua / 1000;
        const bool refused = ((s.rtRaw >> kUsbinCollapseBit) & 0x01u) != 0 ||
                             ((s.rtRaw >> kUsbinUvBit) & 0x01u) != 0;

        if (iclPhase == 0) {
            iclPhase = 1;
            iclTicks = 0;
        }
        if (iclPhase == 1) {
            iclSnk = damSnk(s);
            if (iclSnk > 0) {
                iclLocked = true;
                kprintf("DbyBattery: icl stopped: TYPE_C_SNK_STATUS says a debug-accessory sink "
                      "(SNK_DAM=0x%02x), so smblib_set_icl_current() would configure nothing; "
                      "registers left as they were found\n", static_cast<unsigned>(iclSnk));
                return;
            }
            // smblib_run_aicl() reads POWER_PATH_STATUS and skips the command while the input is
            // suspended (:2691-2700); the same condition is honoured here, so a suspended input is
            // not quietly measured as if it were live.
            if (s.suspend == 0) {
                maskedWrite(kPpidMisc, kAdAiclCmd, 1u << kAiclRerunBit, 1u << kAiclRerunBit, s);
                iclReran = true;
            }
            iclAicl0 = statusMa(kPpidDcdc, kAdIclStatus, s);
            iclCeiling = iclAicl0;
            iclTicks = 0;
            iclPhase = 2;
            kprintf("DbyBattery: icl measure: aicl=%d reran=%d snk=%d apsd=%s plug=%d on=%d "
                  "sus=%d writes=%u ok=%u deny=%u\n", iclAicl0, iclReran ? 1 : 0, iclSnk,
                  apsdName(stat, res & kApsdResultMask), s.plug, s.online, s.suspend,
                  s.wOps, s.wOk, s.wDenied);
            return;
        }
        if (iclPhase == 2) {
            if (++iclTicks < kIclSettleTicks) return;
            const int aicl = statusMa(kPpidDcdc, kAdIclStatus, s);
            const int iclmax = statusMa(kPpidDcdc, kAdIclMax, s);
            int ceiling = aicl > iclmax ? aicl : iclmax;
            if (ceiling < 0) ceiling = kIclLadder[0].ua / 1000;
            iclCeiling = ceiling;
            iclStep = voteForCeiling(iclCeiling);
            iclPhase = 3;
            kprintf("DbyBattery: icl ceiling: aicl=%d iclmax=%d -> %d mA, vote=%s\n", aicl, iclmax,
                  iclCeiling, kIclLadder[iclStep].name);
            return;
        }
        if (iclPhase == 3) {
            if (iclStep == 0) applySdpArm(s); else applyHcArm(kIclLadder[iclStep].ua, s);
            kprintf("DbyBattery: icl apply step=%u name=%s want=%d ceiling=%d apsd=%s "
                  "writes=%u ok=%u deny=%u\n", iclStep, kIclLadder[iclStep].name,
                  kIclLadder[iclStep].ua / 1000, iclCeiling,
                  apsdName(stat, res & kApsdResultMask), s.wOps, s.wOk, s.wDenied);
            if (s.wOps == 0 || (s.wOk == 0 && s.wDenied > 0)) {
                // No channel, or the arbiter's ownership rule refused it, or the window never mapped.
                // Saying so once and stopping is the honest answer; a controller that only counts
                // denials every two seconds is noise.
                iclLocked = true;
                kprintf("DbyBattery: icl stopped: the write path is unavailable (ops=%u denied=%u "
                      "wform=%d)\n", s.wOps, s.wDenied, wForm);
            } else if (s.wOk == 0) {
                iclLocked = true;
                kprintf("DbyBattery: icl stopped: no write answered (ops=%u to=%u er=%u wform=%d); "
                      "registers left as they were found\n", s.wOps, s.wTimeouts, s.wErrors, wForm);
            } else {
                iclGood = iclStep;
                iclTicks = 0;
                iclPhase = 4;
            }
            return;
        }
        if (iclPhase == 4) {
            if (++iclTicks < kIclSettleTicks) return;
            iclPhase = 5;
        }
        if (iclPhase == 5) {
            const int armed = kIclLadder[iclStep].ua / 1000;
            if (!refused) {
                iclPhase = 6;
                kprintf("DbyBattery: icl holding want=%d (rt=0x%02x) ceiling=%d ma=%d\n", armed,
                      s.rtRaw, iclCeiling, s.ma);
                return;
            }
            if (iclStep == 0) {
                applyHwAuto(s);
                iclLocked = true;
                kprintf("DbyBattery: icl rollback: even %d mA collapsed the input (rt=0x%02x); "
                      "HW_AUTO_MODE restored, controller off for this boot\n", armed, s.rtRaw);
                return;
            }
            // One table step down.  The measurement said this cable could give more and the
            // hardware just said it did not, so the value below it in the same table is the one the
            // refusal actually supports.
            --iclStep;
            iclCeiling = kIclLadder[iclStep].ua / 1000;
            iclPhase = 3;
            kprintf("DbyBattery: icl refused want=%d (rt=0x%02x), re-voting %d\n", armed, s.rtRaw,
                  kIclLadder[iclStep].ua / 1000);
            return;
        }
        // hold: on the slow tier only.  Re-read the ceiling so a different cable is re-voted, and
        // step back on a refusal; an echo of the override is not a new ceiling and changes nothing.
        if (iclPhase == 6 && full) {
            const int armed = kIclLadder[iclStep].ua / 1000;
            if (refused) {
                if (iclStep == 0) {
                    applyHwAuto(s);
                    iclLocked = true;
                    kprintf("DbyBattery: icl late rollback: %d mA collapsed the input (rt=0x%02x)\n",
                          armed, s.rtRaw);
                    return;
                }
                --iclStep;
                iclPhase = 3;
                kprintf("DbyBattery: icl late refusal want=%d (rt=0x%02x), re-voting %d\n", armed,
                      s.rtRaw, kIclLadder[iclStep].ua / 1000);
                return;
            }
            const int aicl = statusMa(kPpidDcdc, kAdIclStatus, s);
            const int iclmax = statusMa(kPpidDcdc, kAdIclMax, s);
            const int ceiling = aicl > iclmax ? aicl : iclmax;
            if (ceiling >= 0 && ceiling != iclCeiling) {
                const unsigned vote = voteForCeiling(ceiling);
                kprintf("DbyBattery: icl ceiling moved %d -> %d (iclmax=%d aicl=%d)\n", iclCeiling,
                      ceiling, iclmax, aicl);
                iclCeiling = ceiling;
                if (vote != iclStep) {
                    iclStep = vote;
                    iclPhase = 3;
                }
            }
        }
    }

    // Which request channel this peripheral's writes go through: USBIN and MISC are the only two
    // this file ever names, and each has its own window because each has its own APID.
    volatile uint32_t *rwWindow(uint32_t ppid) const {
        return ppid == kPpidMisc ? rwMisc : rwUsb;
    }

    // One byte of one USID-2 peripheral, and the reason it did not come back.
    //
    // A failure gives up on the FIELD, the way sheng_battmgr_timeout() gives up on one field and lets
    // the next cycle read it again (ShengBattmgr.c:99-106). There is no boot-long latch any more, and
    // that is the fix for "the percentage is different every boot": one arbiter error bit at sample #62
    // of the 2026-11-06 capture (op=1 tx=1 clean=0 to=0 err=1) used to freeze every later read at tx=0,
    // so the published dictionary kept whatever the last live sample said - and which sample is the last
    // live one is not the same number on two boots.
    //
    // DONE with an error bit is the arbiter's own failure: the spelling goes back to the ladder. Only a
    // transaction that never signals DONE at all ends the cycle, and the cycle costs at most
    // kOpFormCount polls per field, never a whole boot.
    Why readByte(uint32_t ppid, uint8_t address, uint8_t *out, Sample &s) {
        if (!obs) return kWhyNoMap;
        const unsigned periph = ppid & 0xffu;
        if (periph >= kApidTable || apid[periph] < 0) return kWhyNoApid;
        const unsigned channel = static_cast<unsigned>(apid[periph]);
        ++s.txns;
        if (form >= 0) {
            const int r = spmiAttempt(channel, kOpForms[static_cast<unsigned>(form)], address, out, s);
            if (r == 1) { ++s.clean; return kWhyOk; }
            if (r == 0) { s.engineSilent = true; return kWhyTimeout; }
            form = -2;
        }
        for (unsigned i = 0; i < kOpFormCount; ++i) {
            const int r = spmiAttempt(channel, kOpForms[i], address, out, s);
            if (r == 1) {
                const bool changed = form != static_cast<int>(i);
                form = static_cast<int>(i);
                ++s.clean;
                if (changed)
                    kprintf("DbyBattery: read opcode=%u sid=%u answered ppid=0x%x addr=0x%x\n",
                          kOpForms[i].opcode, static_cast<unsigned>(kOpForms[i].withSid), ppid,
                          static_cast<unsigned>(address));
                return kWhyOk;
            }
            if (r == 0) { s.engineSilent = true; return kWhyTimeout; }
        }
        return kWhyError;
    }

    // False from the moment a transaction stops answering at all: the rest of the cycle would only cost
    // polls, and its fields are reported as timeouts rather than prettied up.
    bool live(Sample &s, bool &over) {
        if (over) return false;
        if (!s.engineSilent) return true;
        over = true;
        kprintf("DbyBattery: engine silent, cycle %u ends early; the next cycle re-walks the ladder\n",
              samples + 1u);
        return false;
    }

    // Two bytes little endian, plus the vendor's two refusal conditions: the shadow copy must agree
    // (fg-util.c reads both and returns -EINVAL when they do not) and the result must be possible.
    Why readPair16(uint32_t ppid, uint8_t lo, uint8_t hi, uint8_t cpLo, uint8_t cpHi, uint32_t *raw,
                   Sample &s) {
        uint8_t a = 0, b = 0, c = 0, d = 0;
        Why w = readByte(ppid, lo, &a, s);
        if (w == kWhyOk) w = readByte(ppid, hi, &b, s);
        if (w == kWhyOk) w = readByte(ppid, cpLo, &c, s);
        if (w == kWhyOk) w = readByte(ppid, cpHi, &d, s);
        if (w != kWhyOk) return w;
        if (a != c || b != d) return kWhyShadow;
        *raw = (static_cast<uint32_t>(b) << 8) | a;
        return kWhyOk;
    }

    // ---- one pass over the gauge and the charger ---------------------------------------------------

    // `full` is the 60 s tier: everything. The 2 s tier reads the presence, power-path, enable, health,
    // termination and current fields only, and every field it left alone carries kWhySlowOnly, so a fast
    // row can never be mistaken for a cycle whose slow fields timed out.
    void sample(bool full) {
        Sample s = {};
        for (unsigned f = 0; f < kFieldCount; ++f) noteWhy(s, f, full ? kWhyTimeout : kWhySlowOnly);
        s.soc = s.mv = s.ma = s.plug = s.online = s.suspend = s.enable = s.stage = -1;
        s.il = s.iclc = s.iclm = s.icl = s.flt = -1;
        s.cold = s.hot = s.terminalMissing = s.thermMissing = -1;
        // USBIN INT_RT_STS is one byte that carries PLUGIN bit 4, COLLAPSE bit 0, VASHDN bit 1, UV
        // bit 2 and OV bit 3 (smb5-reg.h:236-239), so the presence read below is also the byte the
        // input-current controller judges; keeping it costs the controller no extra transaction.
        s.rtRaw = -1;
        s.apsd = s.apsr = s.want = -1;
        s.engineSilent = false;
        bool over = false;
        uint8_t a = 0, b = 0;
        uint32_t raw = 0;
        Why w;

        // MSOC and its copy, fg-util.c:859-910. The slow tier's field.
        w = full ? readByte(kPpidFgSoc, kAdSoc, &a, s) : kWhySlowOnly;
        if (w == kWhyOk) w = readByte(kPpidFgSoc, kAdSocCp, &b, s);
        if (w != kWhyOk) noteWhy(s, kFieldSoc, w);
        else if (a != b) noteWhy(s, kFieldSoc, kWhyShadow);
        else {
            if (a == 255u) s.soc = 100;
            else if (a == 0u) s.soc = 0;
            else s.soc = static_cast<int>((((a - 1u) * kMsocSlope) + kMsocSpan / 2u) / kMsocSpan + 1u);
            noteWhy(s, kFieldSoc, kWhyOk);
        }

        // Voltage, VBAT against VBAT_CP.
        if (!full) w = kWhySlowOnly;
        else if (!live(s, over)) w = kWhyTimeout;
        else w = readPair16(kPpidFgInfo, kAdVbatt0, kAdVbatt1, kAdVbattCp0, kAdVbattCp1, &raw, s);
        if (w == kWhyOk) {
            const int mv = static_cast<int>(static_cast<uint64_t>(raw) * kVoltNum / kVoltDen);
            if (mv < kVoltMinMv || mv > kVoltMaxMv) noteWhy(s, kFieldV, kWhyRange);
            else { s.mv = mv; noteWhy(s, kFieldV, kWhyOk); }
        } else noteWhy(s, kFieldV, w);

        // Current. Negative means discharging, so the sign is part of the measurement and cannot
        // double as "no value".
        if (!live(s, over)) w = kWhyTimeout;
        else w = readPair16(kPpidFgInfo, kAdIbat0, kAdIbat1, kAdIbatCp0, kAdIbatCp1, &raw, s);
        if (w == kWhyOk) {
            int32_t signedRaw = static_cast<int32_t>(raw);
            if (signedRaw & 0x8000) signedRaw -= 0x10000;              // sign_extend32(raw, 15)
            const int ma = static_cast<int>(static_cast<int64_t>(signedRaw) * kCurrNum / kCurrDen);
            if (ma < -kCurrMaxMa || ma > kCurrMaxMa) noteWhy(s, kFieldI, kWhyRange);
            else { s.ma = ma; noteWhy(s, kFieldI, kWhyOk); }
        } else noteWhy(s, kFieldI, w);

        // Is a cable in? USBIN_PLUGIN_RT_STS bit 4 of the 0x1300 block's INT_RT_STS, which is the
        // vendor's usb_present (smblib_get_prop_usb_present, smb5-lib.c:3495 region).
        if (!live(s, over)) w = kWhyTimeout;
        else w = readByte(kPpidUsb, kAdIntRtSts, &a, s);
        if (w == kWhyOk) {
            s.plug = static_cast<int>((a >> kUsbinPlugBit) & 0x01u);
            s.rtRaw = static_cast<int>(a);
        }
        noteWhy(s, kFieldPlug, w);

        // Is that cable actually feeding the board, and has something suspended the input? Both come out
        // of one byte: POWER_PATH_STATUS. This is the official ExternalConnected, and it is NOT the plug
        // bit - the vendor keeps those apart on purpose (smb5-lib.c:3515-3560 vs usb_present).
        if (live(s, over)) {
            w = readByte(kPpidDcdc, kAdPowerPath, &a, s);
            if (w == kWhyOk) {
                s.online = static_cast<int>(((a >> kUseUsbinBit) & 0x01u) &&
                                            ((a >> kValidInputStsBit) & 0x01u));
                s.suspend = static_cast<int>((a >> kUsbinSuspendStsBit) & 0x01u);
                noteWhy(s, kFieldOnline, w);
                noteWhy(s, kFieldSuspend, w);
            } else {
                noteWhy(s, kFieldOnline, w);
                noteWhy(s, kFieldSuspend, w);
            }
        }

        // Whether the input is suspended by vote rather than by echo, what the input limit was asked
        // to be, what ceiling the source detection allows, what the hardware applied, and where the
        // termination threshold sits. Read whether or not a cable is in, and none of them written:
        // this is the first time anything on this board has looked, and 电充不进去 has exactly four
        // official candidate answers - input_suspend left voted by the last Android pass, an ICL of
        // tens of milliamps, charging disabled, or a float voltage already under the cell.
        if (live(s, over)) {
            w = readByte(kPpidUsb, kAdUsbinCmdIl, &a, s);
            if (w == kWhyOk) s.il = static_cast<int>(a);
            noteWhy(s, kFieldInputCfg, w);
        }
        if (full && live(s, over)) {
            w = readByte(kPpidUsb, kAdUsbinIclCfg, &a, s);
            if (w == kWhyOk) s.iclc = static_cast<int>(a) * static_cast<int>(kIclStepMa);
            noteWhy(s, kFieldIclCfg, w);
        }
        if (full && live(s, over)) {
            w = readByte(kPpidDcdc, kAdIclMax, &a, s);
            if (w == kWhyOk) s.iclm = static_cast<int>(a) * static_cast<int>(kIclStepMa);
            noteWhy(s, kFieldIclMax, w);
        }
        if (full && live(s, over)) {
            w = readByte(kPpidDcdc, kAdIclStatus, &a, s);
            if (w == kWhyOk) s.icl = static_cast<int>(a) * static_cast<int>(kIclStepMa);
            noteWhy(s, kFieldIclNow, w);
        }
        if (full && live(s, over)) {
            w = readByte(kPpidChgr, kAdFloatVolt, &a, s);
            if (w == kWhyOk)
                s.flt = static_cast<int>(kFloatBaseMv) +
                        static_cast<int>(a) * static_cast<int>(kFloatStepMv);
            noteWhy(s, kFieldFloatVolt, w);
        }

        // The charger's health, enable and termination stage — all read, never written.
        // CHARGING_ENABLE_CMD bit 0 means enabled; the voter writes `chg_disable ? 0 : BIT(0)`
        // (smb5-lib.c:1812-1814), so the register mirror is read as the same polarity.
        if (live(s, over)) {
            w = readByte(kPpidChgr, kAdChgSts7, &a, s);
            if (w == kWhyOk) {
                s.cold = static_cast<int>((a >> kTempTooColdBit) & 0x01u);
                s.hot = static_cast<int>((a >> kTempTooHotBit) & 0x01u);
                noteWhy(s, kFieldHealth, w);
            } else noteWhy(s, kFieldHealth, w);
        }
        if (live(s, over)) {
            w = readByte(kPpidChgr, kAdChgEn, &a, s);
            if (w == kWhyOk) { s.enable = static_cast<int>(a & 0x01u); noteWhy(s, kFieldEnable, w); }
            else noteWhy(s, kFieldEnable, w);
        }
        // BATTERY_CHARGER_STATUS_1 bits 2:0. The vendor uses this byte for exactly one thing in the
        // status path: charge_done, i.e. stage == TERMINATE_CHARGE (smblib_get_prop_batt_charge_done,
        // smb5-lib.c:2533-2549). It is not a charging test on this board - see publish().
        if (live(s, over)) {
            w = readByte(kPpidChgr, kAdChgSts1, &a, s);
            if (w == kWhyOk) { s.stage = static_cast<int>(a & 0x07u); noteWhy(s, kFieldStage, w); }
            else noteWhy(s, kFieldStage, w);
        }

        // Battery and thermistor presence. BatteryInstalled derives from this and never from a gauge
        // that happened to return nothing.
        if (!live(s, over)) {
            noteWhy(s, kFieldTerminal, full ? kWhyTimeout : kWhySlowOnly);
            noteWhy(s, kFieldTherm, full ? kWhyTimeout : kWhySlowOnly);
        } else if (!full) {
            noteWhy(s, kFieldTerminal, kWhySlowOnly);
            noteWhy(s, kFieldTherm, kWhySlowOnly);
        } else {
            w = readByte(kPpidPdet, kAdIntRtSts, &a, s);
            if (w == kWhyOk) {
                s.terminalMissing = static_cast<int>((a >> kBatMissingShift) & 0x01u);
                s.thermMissing = static_cast<int>((a >> (kBatMissingShift + 1u)) & 0x01u);
            }
            noteWhy(s, kFieldTerminal, w);
            noteWhy(s, kFieldTherm, w);
        }

        // The controller is the last thing in the cycle: it judges the bytes this cycle read and its
        // own transactions are counted separately, so a row always says what was asked and what the
        // real-time status said afterwards.
        runIcl(s, full);

        ++samples;
        // Publish once per cycle and no more often: upstream's only publish point is the end of a cycle
        // (ShengBattery.cpp:150). That is the half of the charging-icon fix that has nothing to do with
        // which bits are read - a load transient shorter than a cycle cannot reach powerd at all.
        publish(s);
        logSample(s, full);
    }

    // The vendor's rule, transcribed, not re-derived. From the compiled branch of
    // smblib_get_prop_batt_status() in this tablet's own kernel (smb5-lib.c:2111-2162, Huawei's branch
    // because merge_kona_defconfig:5912 sets CONFIG_HUAWEI_POWER_EMBEDDED_ISOLATION=y):
    //
    //   status = (usb_present && !usb_icl_change_irq_enabled && !input_suspend)
    //            ? (chg_enable ? CHARGING : NOT_CHARGING) : DISCHARGING
    //   with health COLD or OVERHEAT (and OVERVOLTAGE, conditionally) rewritten to NOT_CHARGING.
    //
    // The SMB5 3-bit regulation stage is not in this rule - which is exactly what was wrong here. The
    // code this file carried tested `stage >= Trickle && stage <= Taper` and then `stage != Terminate`;
    // a load step such as the brightness slider walks that stage through INHIBIT or PAUSE for a sample
    // at a time, so the icon switched off while the cell was still taking current. The stage stays good
    // for one thing only: saying the cell is full.
    //
    // Of the terms that can produce CHARGING, two are invisible from here: the direct-charge stage
    // window (direct_charge_get_stage_status(), a driver-internal state machine) and the wireless
    // adapter type (chg->real_charger_type). Dropping a term from a disjunction can only answer
    // NOT_CHARGING where the vendor would say CHARGING, which is the safe direction for an icon - it
    // can lag a real charge, it cannot invent one.
    //
    // usb_icl_change_irq_enabled is not dropped but read as false, and that is the vendor's own steady
    // state rather than an assumption: SMB5's smblib_set_usb_suspend() suspends the ICL-change
    // interrupt through icl_irq_disable_votable (smb5-lib.c:767-785) and never touches this field, so
    // it keeps its zero-initialised false unless a writer sets POWER_SUPPLY_PROP_HIZ_MODE
    // (qpnp-smb5.c:2322). The third term is therefore the live path for a wired charge, which is what
    // makes it worth transcribing and what the 2026-11-06 icon loss had removed.
    //
    // The OVERVOLTAGE health term is left out. smblib_get_prop_batt_health() concludes OVERVOLTAGE
    // from the raw BAT_OV bit only after comparing the gauge's VOLTAGE_NOW with the float-voltage
    // votable plus 40 mV (smb5-lib.c:2413-2429); with no votable to read, honouring the bit would
    // assert a state the vendor itself refuses to. COLD (bit 2) and OVERHEAT (bit 3) need nothing but
    // STATUS_7, and both are read.
    bool chargingOf(const Sample &s) const {
        if (!has(s, kFieldPlug) || !has(s, kFieldEnable) || !has(s, kFieldInputCfg) ||
            !has(s, kFieldHealth))
            return false;
        // The vendor's rule, transcribed and nothing added to it. Under
        // CONFIG_HUAWEI_POWER_EMBEDDED_ISOLATION (merge_kona_defconfig:5912) the CHARGING branch of
        // smblib_get_prop_batt_status() is (smb5-lib.c:2129-2157):
        //     (dc stage in [ADAPTER_DETECT, CHARGE_DONE]) || wireless ||
        //     (usb_present && !chg->usb_icl_change_irq_enabled && !chg->input_suspend)  (:2137)
        //       -> chg_enable ? CHARGING : NOT_CHARGING
        //     health OVERVOLTAGE/OVERHEAT/COLD -> NOT_CHARGING
        // There is no fuel-gauge current anywhere in it, and the earlier `ma <= 0` veto that used to
        // sit here was the owner's 2026-11-06 complaint: on a 500 mA host port IBAT crosses zero with
        // every load transient, so the badge was erased a cycle after the AC notification raised it.
        // The gauge still answers for the capacity and the voltage; it just does not get a vote on
        // whether the charger is charging.
        //
        // chg->usb_icl_change_irq_enabled is declared at smb5-lib.h:582 and, grepped across the whole
        // qcom power tree, the smb5 driver writes it in exactly one place: qpnp-smb5.c:2323, a
        // power-supply property setter. It is not a register and it starts zero at probe, so in a boot
        // with no charger driver the term is false by construction rather than by assumption. The two
        // Huawei direct-charge terms are absent because direct_charge_get_stage_status() is a software
        // stage machine, and POWER_SUPPLY_TYPE_WIRELESS needs the wireless RX driver - neither is a
        // byte a host can read.
        const bool inputSuspend = ((s.il >> kInputSuspendBit) & 1) != 0;
        return s.plug != 0 && !inputSuspend && s.enable != 0 && s.cold == 0 && s.hot == 0;
    }

    // Upstream's external is the USB-online answer (ShengBattmgr.c:154, smb5-lib.c:3556-3560), but one
    // of its two terms is an enable mirror that nothing in this boot writes, so it cannot fall when
    // the cable does. The vendor keeps the real-time presence bit apart for exactly that case, so
    // external is the AND of the two hardware terms and says "no adapter" the moment plug does.
    bool externalOf(const Sample &s) const {
        // Presence, in the vendor's own sense: smblib_get_prop_usb_present() is USBIN INT_RT_STS
        // bit 4 and nothing else (smb5-lib.c:3499-3513), and it is that term - not usb_online - that
        // the charging test at :2137 gates on.  POWER_PATH_STATUS's USE_USBIN bit is the mirror of
        // the usbin_path_enable votable, i.e. a bit the charger driver writes; smblib_get_prop_-
        // usb_online() (:3515-3560) reads it because in Android the driver that answers for it is
        // running.  On this tablet with no charger driver in the boot it is whoever set it last, and
        // the owner's 2026-11-06 report is what that costs: the badge followed the first plug and
        // unplug pair, then a replug left the mirror down while VBUS came back, so external went
        // false and the charging icon vanished on a cable that was in.  online= stays in the row as
        // the mirror's own answer; it is simply no longer what says a cable is feeding the board.
        return has(s, kFieldPlug) && s.plug != 0;
    }

    // FullyCharged = feeding the board and (the vendor's charge_done stage, or 100 percent), which is
    // ShengBattmgr.c:158 with smb5-lib.c:2549's TERMINATE_CHARGE in place of the firmware's status 4.
    bool fullOf(const Sample &s) const {
        return externalOf(s) && ((has(s, kFieldStage) && s.stage == kStageTerminate) ||
                                 (has(s, kFieldSoc) && s.soc == 100));
    }

    // What powerd sees. Only fields with a reading are set — IOPMPowerSource's dictionary starts empty
    // and updateStatus() publishes exactly the keys that were set, so leaving a setter out is how
    // "unknown" is said. registerService() happens once, after the first dictionary is in place, the
    // way upstream's publish() does it (ShengBattery.cpp:136-141).
    void publish(const Sample &s) {
        if (!has(s, kFieldSoc) && !has(s, kFieldV) && !has(s, kFieldI) && !has(s, kFieldStage) &&
            !has(s, kFieldPlug) && !has(s, kFieldOnline) && !has(s, kFieldTerminal)) {
            return;   // nothing answered, so there is no power source to describe
        }
        // Upstream's order of dependency (ShengBattmgr.c:154-158): the cable decides everything else,
        // `charging = external && status == 1`. chargingOf() below is the vendor's own CHARGING-vs-
        // NOT_CHARGING rule (smb5-lib.c:2129-2157), in which the enable mirror only ever gets to say
        // "inhibited"; it cannot say "charging" with no input, which is why it sits behind this term.
        const bool external = externalOf(s);
        const bool charging = external && chargingOf(s);
        const bool full = fullOf(s);

        if (has(s, kFieldTerminal)) {
            setBatteryInstalled(s.terminalMissing == 0);
            if (s.terminalMissing) {
                const OSSymbol *error = OSSymbol::withCString("BatteryMissing");
                if (error) { setErrorCondition(const_cast<OSSymbol *>(error)); error->release(); }
            }
        }
        // The three presence booleans go out on every cycle, which is what upstream does with them
        // (ShengBattery.cpp:96-98) on a snapshot where an unreported property decodes as false rather
        // than as "keep the last value" (ShengBattmgr.c:152-159). Gating them on their reads is the
        // defect being fixed: a cycle in which the charger block did not answer left the previous
        // ExternalConnected/IsCharging=true pair published, and the only thing that could clear it was
        // another answered cycle - the icon outlived the cable. The numbers keep the opposite rule, an
        // unanswered field stays out of the dictionary, because those are the ones that must not be
        // invented.
        setExternalConnected(external);
        setExternalChargeCapable(external);
        setIsCharging(charging);
        setPSFlag("FullyCharged", full);
        if (has(s, kFieldSoc)) {
            setCurrentCapacity(static_cast<unsigned>(s.soc));
            setMaxCapacity(100);
        }
        // Upstream's critical (ShengBattmgr.c:159): on the battery and at or under two percent.
        if (has(s, kFieldTerminal) && has(s, kFieldOnline) && has(s, kFieldSoc))
            setAtCriticalLevel(s.terminalMissing == 0 && !external && s.soc <= 2);
        // Time to empty and time to full both live in the gauge's interleaved SRAM, read through
        // fg_sram_read() (qpnp-fg-gen4.c:874-878, :913-917), which this file must not write. 0xffff is
        // not a sentinel of mine: it is upstream's own unknown for this key (ShengBattery.cpp:105-106).
        setTimeRemaining(0xffffu);
        if (has(s, kFieldV)) setVoltage(static_cast<unsigned>(s.mv));
        if (has(s, kFieldI)) {
            setAmperage(s.ma);
            // The telemetry daemons read InstantAmperage rather than Amperage; it is the same
            // measurement, which is why upstream publishes both (ShengBattery.cpp:118).
            setPSNumber("InstantAmperage", s.ma);
        }

        // Upstream's gate, same shape as ShengBattery.cpp:139 (`!published && !s.charge_known`): do
        // not put a power source in front of powerd before the gauge has actually answered for its
        // state of charge. With CurrentCapacity absent, iOS falls back to its own voltage-curve guess,
        // which is why the first boots of this port showed a different number every time.
        if (!published && !has(s, kFieldSoc)) return;
        if (!published) {
            published = true;
            registerService();
            native_battery_stats[0] = 4;
            kprintf("DbyBattery: published soc=%d mv=%d ma=%d plug=%d on=%d en=%d sus=%d st=%d\n",
                  s.soc, s.mv, s.ma, s.plug, s.online, s.enable, s.suspend, s.stage);
        }
        updateStatus();

        // Telling the registry is not telling the OS: updateStatus()'s message has no consumer in this
        // kernel (IOPMPowerSource.cpp:211 is the only sender, IOPM.h:865 the only other mention), and the
        // path that does reach userland is the root domain's AC branch, which posts
        // IOUserServer::powerSourceChanged() (IOPMrootDomain.cpp:8472, IOUserServer.cpp:5496-5509).
        // receivePowerNotification() queues onto pmPowerStateQueue (:8311) rather than doing the work on
        // this work loop, so the call is safe from the timer. Driven by presence, on an edge only.
        if (acTold != (external ? 1 : 0)) {
            IOPMrootDomain *root = getPMRootDomain();
            if (root) {
                root->receivePowerNotification(kIOPMSetACAdaptorConnected |
                                               (external ? kIOPMSetValue : 0u));
                acTold = external ? 1 : 0;
            }
        }

        // Upstream's indices (ShengBattery.cpp:34-37), re-pointed at this transport: [4] temperature,
        // [10] adapter type, [11] full mAh, [12] design mAh and [13] cycles are the five the header
        // says this path cannot read, so they stay -1 rather than taking a plausible value.
        // A 2 s cycle does not read the gauge numbers, and writing -1 over a measured one would make the
        // array disagree with the dictionary the UI is showing. Slots that were not read keep their value.
        if (has(s, kFieldSoc)) native_battery_stats[1] = s.soc;
        if (has(s, kFieldV)) native_battery_stats[2] = s.mv;
        if (has(s, kFieldI)) native_battery_stats[3] = s.ma;
        native_battery_stats[4] = -1;
        // Upstream's bit meanings unchanged (ShengBattery.cpp:131): present, external, charging, full,
        // notifications enabled. Bit 4 is 0 because no PMIC interrupt path reaches this service - the
        // periodic cycle is the only thing that refreshes it - and bit 2 is now the vendor's rule rather
        // than the stage window it used to restate.
        native_battery_stats[5] = (has(s, kFieldTerminal) && !s.terminalMissing)
            | (external ? 1 : 0) << 1
            | (charging ? 1 : 0) << 2
            | (full ? 1 : 0) << 3;
        native_battery_stats[6] = static_cast<int32_t>(samples);
        native_battery_stats[7] = static_cast<int32_t>(s.clean);
        native_battery_stats[8] = static_cast<int32_t>(s.timeouts | (s.errors << 16));
        native_battery_stats[9] = 0;     // no PMIC interrupt path is wired to this service yet
        native_battery_stats[10] = -1;
        native_battery_stats[11] = -1;
        native_battery_stats[12] = -1;
        native_battery_stats[13] = -1;
        native_battery_stats[14] = static_cast<int32_t>(countWhy(s, kWhyShadow) |
                                                        (countWhy(s, kWhyRange) << 16));
        native_battery_stats[15] = static_cast<int32_t>(countWhy(s, kWhyNoApid) |
                                                        (countWhy(s, kWhyNoMap) << 16));
    }

    static unsigned countWhy(const Sample &s, uint32_t reason) {
        unsigned n = 0;
        for (unsigned field = 0; field < kFieldCount; ++field) {
            if (s.why[field] == reason) ++n;
        }
        return n;
    }

    void setPSFlag(const char *key, bool value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        if (symbol) setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
        OSSafeReleaseNULL(symbol);
    }

    void setPSNumber(const char *key, int64_t value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        OSNumber *number = OSNumber::withNumber(static_cast<unsigned long long>(value), 32);
        if (symbol && number) setPSProperty(symbol, number);
        OSSafeReleaseNULL(number);
        OSSafeReleaseNULL(symbol);
    }

    // One line per sample, on the console this board already carries. Every field that did not answer
    // is named with its reason, so a missing number and a zero cannot be confused by a reader.
    // One row per cycle. `why=` is the reason byte of every field in table order, one hex digit each,
    // so 0000000000000000 is a cycle in which all sixteen fields answered; the named pairs after it
    // are only the fields that did not. The on/plug/en/sus/cfg/icl/flt/st/chg/full words are the
    // vendor's own status inputs and its answer, printed so a charging-icon judgement needs no second
    // instrument and a "why will it not charge" judgement needs no write.
    void logSample(const Sample &s, bool full) const {
        static const char *const names[kFieldCount] = {"soc", "v", "i", "plug", "on", "sus", "cfg",
                                                       "iclc", "iclm", "icl", "flt", "en", "health",
                                                       "stage", "term", "therm"};
        kprintf("DbyBattery: #%u tier=%d op=%d tx=%u clean=%u to=%u err=%u why=", samples,
              full ? 1 : 0,
              form >= 0 ? static_cast<int>(kOpForms[static_cast<unsigned>(form)].opcode) : -1, s.txns,
              s.clean, s.timeouts, s.errors);
        for (unsigned field = 0; field < kFieldCount; ++field) {
            kprintf("%x", static_cast<unsigned>(s.why[field]) & 0xfu);
        }
        kprintf(" soc=%d mv=%d ma=%d plug=%d on=%d sus=%d en=%d st=%d cold=%d hot=%d chg=%d full=%d"
                " cfg=0x%02x iclc=%d iclm=%d icl=%d flt=%d",
              s.soc, s.mv, s.ma, s.plug, s.online, s.suspend, s.enable, s.stage, s.cold, s.hot,
              chargingOf(s) ? 1 : 0, fullOf(s) ? 1 : 0, s.il, s.iclc, s.iclm, s.icl, s.flt);
        // The input-current half of the row, only when the write tier is on. want= is what this
        // service asked the source for, apsd/aprd are the two APS-D bytes decoded and raw, rt= is
        // USBIN INT_RT_STS in full, and step/good/ph/latch say where the controller is. ceiling/aicl0
        // are the measured limit and the pre-re-run copy of AICL_ICL_STATUS in mA, snk is the DAM field
        // of TYPE_C_SNK_STATUS (-1 unread), reran says the AICL re-run was issued, and ib+/ib- count
        // the sign of IBAT across the cycles, which is the outcome no write of ours produced.
        // w[...] is the
        // request channel's own tally: ops/ok/timeout/error plus the denials, which are the ownership
        // rule refusing us rather than the hardware misbehaving.
        if (iclWrite) {
            kprintf(" want=%d apst=%d aprd=0x%02x rt=0x%02x step=%u good=%u ph=%u latch=%d"
                    " ceiling=%d aicl0=%d snk=%d reran=%d ib+=%u ib-=%u"
                    " w[op=%d ops=%u ok=%u to=%u er=%u deny=%u]",
                  s.want, s.apsd, s.apsr < 0 ? 0xffu : static_cast<unsigned>(s.apsr),
                  s.rtRaw < 0 ? 0u : static_cast<unsigned>(s.rtRaw), iclStep, iclGood, iclPhase,
                  iclLocked ? 1 : 0, iclCeiling, iclAicl0, iclSnk, iclReran ? 1 : 0,
                  ibPos, ibNeg,
                  wForm >= 0 ? static_cast<int>(kWriteForms[static_cast<unsigned>(wForm)].opcode)
                             : -1,
                  s.wOps, s.wOk, s.wTimeouts, s.wErrors, s.wDenied);
        }
        for (unsigned field = 0; field < kFieldCount; ++field) {
            if (s.why[field] == kWhyOk) continue;
            kprintf(" %s=%u", names[field], static_cast<unsigned>(s.why[field]));
        }
        kprintf("\n");
    }

    // ---- the tables: which APID carries which peripheral ------------------------------------------
    //
    // The scan bound is the vendor driver's, not a guess: pmic_arb_read_apid_map_v5 stops when the
    // computed offset leaves the mapped window (spmi-pmic-arb.c:910-912) and core_size is the DT `reg`
    // size (:1200). With a 0x1100-byte core window and the v5 base of 0x900 that is
    // (0x1100-0x900)/4 = 512 entries, which is also PMIC_ARB_MAX_PERIPHS (:92), so both bounds are
    // derived and the smaller of the two is what the loop uses.
    bool census() {
        for (unsigned i = 0; i < kApidTable; ++i) {
            apid[i] = -1;
            writeEe[i] = 0xffu;   // unknown until the ownership table answers for this channel
        }
        volatile uint32_t *core = mapWindow(kCoreBase, kCoreSize, &coreMap);
        if (!core) {
            kprintf("DbyBattery: no core window at 0x%x\n", static_cast<unsigned>(kCoreBase));
            return false;
        }
        // Identity first: a window that answers garbage never gets a second window mapped and never
        // gets a table walked. A zero read (window not answering) and 0xffffffff (dead bus) are both
        // outside the v5 range, which is the point of the test.
        const uint32_t version = rd32(reinterpret_cast<uintptr_t>(core), kRegVersion);
        if (version < kVersionV5Min || version >= kVersionMaxExclusive) {
            kprintf("DbyBattery: arbiter version %08x is not v5, stopping\n", version);
            return false;
        }
        volatile uint32_t *cnfg = mapWindow(kCnfgBase, kCnfgSize, &cnfgMap);
        if (!cnfg) kprintf("DbyBattery: cnfg window unreadable, ownership not reported\n");

        unsigned entries = static_cast<unsigned>((kCoreSize - kApidMapBase) / 4u);
        if (entries > kMaxApids) entries = kMaxApids;
        unsigned usid2 = 0, own = 0, irqOwner = 0, fgWriteEe = 0, fgIrq = 0;
        for (unsigned i = 0; i < entries; ++i) {
            const uint32_t map = rd32(reinterpret_cast<uintptr_t>(core), kApidMapBase + 4u * i);
            if (!map) continue;                                       // spmi-pmic-arb.c:915-916
            const uint32_t ppid = (map >> kPpidShift) & kPpidMask;    // :917
            if ((ppid >> 8) != kPmicUsid) continue;
            ++usid2;
            const unsigned irq = (map & kApidIrqOwnerBit) ? 1u : 0u;  // :46 / :918
            if (irq) ++irqOwner;
            int owner = -1;
            if (cnfg) {
                owner = static_cast<int>(rd32(reinterpret_cast<uintptr_t>(cnfg),
                                             kOwnershipBase + 4u * i) & kWriteEeMask);  // :920-922
            }
            // One channel per peripheral id: this EE's own if there is one, else the first mapping,
            // which is what "First PPID mapping or one for this EE" (:930-932) comes down to when a
            // single EE is asking. The ownership word is why a transaction can come back DENIED: a
            // non-zero value means another EE wrote the table for this channel.
            const unsigned periph = ppid & 0xffu;
            if (periph < kApidTable && (apid[periph] < 0 || owner == static_cast<int>(kOwnEe))) {
                apid[periph] = static_cast<int>(i);
                writeEe[periph] = owner < 0 ? 0xffu : static_cast<unsigned>(owner);
                if (owner == static_cast<int>(kOwnEe)) ++own;
                if (ppid == kPpidFgSoc) { fgWriteEe = owner < 0 ? 0xffu : static_cast<unsigned>(owner);
                                          fgIrq = irq; }
            }
        }
        // usbwe/miscwe are the two write-owner cells this boot consumes: whether the request channel
        // for the USBIN block and the one for the MISC block belong to this EE. Anything other than
        // ee0 there is spmi-pmic-arb.c's -EPERM (:1007-1011) and this file will not fight it.
        kprintf("DbyBattery: arb=%08x slots=%u usid2=%u ee%u-owned=%u irq-owner=%u fg[wee=%u iq=%u] "
              "chgr=%d dcdc=%d usb=%d pdet=%d info=%d usbwe=%u misc=%d miscwe=%u\n",
              version, entries, usid2, kOwnEe, own, irqOwner, fgWriteEe, fgIrq,
              apid[kPpidChgr & 0xffu], apid[kPpidDcdc & 0xffu], apid[kPpidUsb & 0xffu],
              apid[kPpidPdet & 0xffu], apid[kPpidFgInfo & 0xffu],
              static_cast<unsigned>(writeEe[kPpidUsb & 0xffu]), apid[kPpidMisc & 0xffu],
              static_cast<unsigned>(writeEe[kPpidMisc & 0xffu]));
        if (usid2 == 0) {
            kprintf("DbyBattery: no APID for USID 2: the battery PMIC is invisible to this EE\n");
            return false;
        }
        if (apid[kPpidFgSoc & 0xffu] < 0 && apid[kPpidChgr & 0xffu] < 0) {
            kprintf("DbyBattery: USID 2 is mapped but neither the gauge nor the charger has a channel\n");
            return false;
        }
        obs = mapWindow(kObsBase, kObsSize, &obsMap);
        if (!obs) {
            kprintf("DbyBattery: observer window did not map, no transaction can be issued\n");
            return false;
        }
        return true;
    }

    static void tickAction(OSObject *owner, IOTimerEventSource *sender) {
        DbyBattery *self = OSDynamicCast(DbyBattery, owner);
        if (!self) return;
        self->sample(true);
        sender->setTimeoutMS(kRefreshMs);
    }

    static void statusTickAction(OSObject *owner, IOTimerEventSource *sender) {
        DbyBattery *self = OSDynamicCast(DbyBattery, owner);
        if (!self) return;
        self->sample(false);
        sender->setTimeoutMS(kStatusMs);
    }

public:
    // The census runs here, on the caller's thread, because it is a table walk with no transaction in
    // it; the first sample, which is the one that publishes, runs on the timer.
    bool startService() {
        loop = IOWorkLoop::workLoop();
        timer = IOTimerEventSource::timerEventSource(this, &DbyBattery::tickAction);
        statusTimer = IOTimerEventSource::timerEventSource(this, &DbyBattery::statusTickAction);
        if (!loop || !timer || !statusTimer ||
            loop->addEventSource(timer) != kIOReturnSuccess ||
            loop->addEventSource(statusTimer) != kIOReturnSuccess) {
            native_battery_stats[0] = -2;
            return false;
        }
        if (!census()) {
            native_battery_stats[0] = -3;
            return false;
        }
        native_battery_stats[0] = 2;   // the census ran: this EE can address the battery PMIC
        // dby-battery=2 is the write tier. The owner's instruction of 2026-11-06 was explicit about
        // its boundary: raising the input current limit through the PMIC's charger configuration is
        // allowed, inventing a mechanism is not - so every register, bit, value, order and rollback
        // state above is quoted from this tablet's own kernel tree, and =1 still means this file
        // writes nothing and maps no request channel at all.
        unsigned tier = 0;
        PE_parse_boot_argn("dby-battery", &tier, sizeof tier);
        iclWrite = tier >= 2;
        kprintf("DbyBattery: tier=%u writes=%d\n", tier, iclWrite ? 1 : 0);
        timer->setTimeoutMS(kFirstMs);
        // After the first full cycle, so the first publish is still the one that carries the gauge.
        statusTimer->setTimeoutMS(kFirstMs + kStatusMs);
        return true;
    }
};
OSDefineMetaClassAndStructors(DbyBattery, IOPMPowerSource)

// Called by BatteryProbe.cpp once this file's classes are registered. The object stays attached to the
// platform: release() at the end drops the constructor reference and the platform's attachment keeps
// it alive, which is upstream's sheng_battery_start() (ShengBattery.cpp:227-241) unchanged in shape.
extern "C" void dby_battery_start(void) {
    DbyBattery *battery = OSTypeAlloc(DbyBattery);
    if (!battery || !battery->init() || !battery->attach(IOService::getPlatform())) {
        if (battery) battery->release();
        native_battery_stats[0] = -1;
        return;
    }
    native_battery_stats[0] = 1;       // the class is up and attached
    if (!battery->startService()) {
        battery->detach(IOService::getPlatform());
        battery->release();
        return;                        // startService() recorded which step failed
    }
    // A first sample that finds no number at all leaves the service un-published, by design: stage 2
    // says the census ran, and a later sample can still get to 4.
    battery->release();
}
