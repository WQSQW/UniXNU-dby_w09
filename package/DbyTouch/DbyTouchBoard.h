/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * DbyTouchBoard.h — DBY-W09 (Huawei MatePad 11, SM8250/kona) touch board facts.
 *
 * EVERY number in this file carries a source tag on the same line or the line
 * above it.  Tags used here:
 *
 *   DTBO    _scratch/analysis/dtbo-0.dts  (overlay; identical in dtbo-0..8)
 *   BASE    _scratch/analysis/base-stock.dts (stock boot.img appended DTB)
 *   MEASURED an actual observation from this handset (log/command recorded)
 *   DOC     unixnu.com page shipped from dby-port/UniXNU-full/docs (cite path)
 *
 * Anything that is still unknown is written as DBY_TOUCH_TODO_* and is listed in
 * README.md §"还缺哪些数值".  Do not replace a TODO with a plausible-looking
 * number: the touch block on this board has four independent unknown layers
 * (GPIO ownership, SPI bring-up, IC protocol, report scaling) and a wrong
 * constant in any of them looks exactly like "touch does not work".
 */

#ifndef DBY_TOUCH_BOARD_H
#define DBY_TOUCH_BOARD_H

/* ------------------------------------------------------------------ */
/* 1. Transport: Qualcomm GENI/QUPv3 serial engine 14, SPI mode        */
/* ------------------------------------------------------------------ */

/* BASE:22307  spi@880000 { reg = <0x880000 0x4000>; reg-names = "se_phys"; }
 * BASE:26673  qupv3_se14_spi = "/soc/spi@880000"   → this is SE index 14 of
 *             wrapper qupv3_2, not "SE 0x88". */
/* NOTE (2026-11-01): this was 0x08800000 until today - one digit too many, sixteen times the real
 * address - and every engine access went to empty space, which is why the engine identity words
 * always read zero and writes "did not stick".  The wrapper (0x008C0000) was always correct, which
 * is what made the board look half-alive.  The value must match the DT `reg` above exactly. */
#define DBY_TOUCH_SE_BASE            0x00880000ULL
#define DBY_TOUCH_SE_SIZE            0x00004000ULL
#define DBY_TOUCH_SE_INDEX           14

/* BASE:22139  qcom,qupv3_2_geni_se@8c0000 { reg = <0x8c0000 0x2000>; }
 * The QUP wrapper only needs its HW version register read (GeniSpi.h:77). */
/* GCC clock controller, QUPv3 wrapper 2 (the wrapper that owns SE14, the touch engine).
 *
 * Source: mainline drivers/clk/qcom/gcc-sm8250.c, transcribed into work/refs/kona-qupv3-se14-clocks.md
 * on 2026-11-01.  Note this is NOT the SM8550 layout the sheng board header carries: on kona the
 * QUP vote register is 0x52010 (sheng: 0x52008) and the wrapper-2 halt registers are 0x23270/0x23278
 * and 0x1e004/0x1e008/0x1e00c.  Copying the sheng values here would write the wrong registers.
 *
 * Vote bits in DBY_TOUCH_GCC_VOTE: core=0, s_ahb=1, m_ahb=2, core_2x=3, wrap2_s0=4.
 * A branch whose CBCR has the hardware-clock-gating bit (bit 1) may legitimately show CLK_OFF while
 * idle; a branch without it must have CLK_OFF clear once voted (the same rule TouchProbe.cpp:399-406
 * applies on sheng). */
#define DBY_TOUCH_GCC_BASE           0x00100000ULL
#define DBY_TOUCH_GCC_SIZE           0x00054000ULL   /* covers 0x1e00c, 0x23278 and 0x52018 */
/* The SE's own root clock.  The five vote bits in 0x52010 only enable *branches*; the branch for
 * wrap2_s0 has the RCG gcc_qupv3_wrap2_s0_clk_src as its parent (vendor gcc-kona.c:920-929) and
 * enabling a branch does not start its parent.  A dead parent also leaves CLK_OFF reading 0, which
 * is why polling the branch CBCR could never prove the clock existed.
 *
 * Enabling it needs three writes to the RCG command register, in this order:
 *   CMD |= BIT(1)      force-enable the root (CMD_ROOT_EN)
 *   CFG  = value       source + divider + HW_CLK_CTRL (BIT(20))
 *   CMD |= BIT(0)      CMD_UPDATE, which self-clears when the new configuration is latched
 *   CMD &= ~BIT(1)     release the force-enable; leaving it set pins the clock on and confuses the
 *                      clock framework for every later user of this root
 * 0x00100001 selects CXO (19.2 MHz) with DIV=1, the zero-risk configuration; the vendor runs this
 * SE at 50 MHz (0x0010060B off GPLL0), which needs the GPLL0 vote first. */
#define DBY_TOUCH_GCC_SE_RCG_CMD     0x1E010U
#define DBY_TOUCH_GCC_SE_RCG_CFG     0x1E014U
#define DBY_TOUCH_GCC_RCG_ROOT_EN    0x2U
#define DBY_TOUCH_GCC_RCG_UPDATE     0x1U
#define DBY_TOUCH_GCC_SE_RCG_CXO_19M 0x00100001U
/* Only the fields written above: DIV[4:0] | SRC_SEL[10:8] | HW_CLK_CTRL[20].  Comparing the whole
 * word would also require MODE and the reserved bits to read back identically. */
/* DIV[7:0] | SRC_SEL[10:8] | MODE[13:12] | HW_CLK_CTRL[20]: only the documented fields, so
 * a read-only or reserved bit cannot fail the compare. */
#define DBY_TOUCH_GCC_RCG_CFG_MASK   0x001037FFU
/* CMD bit31 is ROOT_OFF: the cheap liveness bit mainline's is_enabled() checks. */
#define DBY_TOUCH_GCC_RCG_ROOT_OFF   0x80000000U

#define DBY_TOUCH_GCC_VOTE           0x52010U
#define DBY_TOUCH_GCC_GPLL0_ENABLE   0x52018U
#define DBY_TOUCH_GCC_HALT_M_AHB     0x1e004U
#define DBY_TOUCH_GCC_HALT_S_AHB     0x1e008U
/* The sibling engine SE15's own branch (gcc_qupv3_wrap2_s1_clk: vote bit 5 of 0x52010, halted
 * status 0x1e13c, vendor gcc-kona.c:3030-3046).  This driver never votes it, so before reading
 * anything inside SE15 its branch has to be checked: reading an engine whose branch was never
 * enabled is the 2026-10-31 interconnect hang. */
#define DBY_TOUCH_GCC_HALT_S1        0x1E13CU
#define DBY_TOUCH_GCC_HALT_S0        0x1e00cU
#define DBY_TOUCH_GCC_HALT_CORE      0x23270U
#define DBY_TOUCH_GCC_HALT_CORE_2X   0x23278U
#define DBY_TOUCH_VOTE_WRAP2_ALL     0x1FU          /* core | s_ahb | m_ahb | core_2x | s0 */
/* The SE's own branch only: gcc_qupv3_wrap2_s0_clk has enable_mask BIT(4) of 0x52010 (vendor
 * gcc-kona.c:3011-3026).  Toggling exactly this bit is how the vote-to-branch wiring is tested. */
#define DBY_TOUCH_VOTE_WRAP2_S0      0x10U          /* bit 4 only: the SE's own branch */
#define DBY_TOUCH_CBCR_CLK_OFF       0x80000000U
#define DBY_TOUCH_CBCR_HWCG          0x00000002U

#define DBY_TOUCH_QUP_WRAPPER_BASE   0x008C0000ULL
#define DBY_TOUCH_QUP_WRAPPER_SIZE   0x00002000ULL
#define DBY_TOUCH_QUPV3_HW_VER_REG   0x00000004U  /* GeniSpi.h: QUPV3_HW_VER_REG */

/* BASE:22314  interrupts = <0x00 0x175 0x04>  → GIC SPI 0x175 = 373, level-high.
 * This is the *serial engine* interrupt (TX/RX completion), NOT the touch IRQ.
 * Do not confuse it with the THP irq_gpio (GPIO 39) — panel-touch-analysis.md §4
 * calls this out explicitly. */
#define DBY_TOUCH_SE_IRQ            373

/* BASE:22310  clocks = <0x17 0x78 0x17 0x88 0x17 0x89>  (gcc phandle 0x17)
 *   0x78 = GCC_QUPV3_WRAP2_S0_CLK (se-clk), 0x88 = m-ahb, 0x89 = s-ahb.
 * The TransportCore must vote these before any SE register read. */
#define DBY_TOUCH_GCC_SE_CLK_INDEX    0x78U
#define DBY_TOUCH_GCC_M_AHB_INDEX     0x88U
#define DBY_TOUCH_GCC_S_AHB_INDEX     0x89U

/* BASE:22315  spi-max-frequency = <0x2faf080> = 50 MHz is the CONTROLLER ceiling.
 * DTBO:10059  huawei_thp@10 { spi-max-frequency = <0x989680>; } = 10 MHz.
 * DTBO:10116/10129  novatech/ilitek sub-node override = <0xf42400> = 16 MHz.
 * Runtime value is whatever THP actually programmed; start at 10 MHz. */
#define DBY_TOUCH_CONTROLLER_MAX_HZ  50000000U
#define DBY_TOUCH_THP_CLOCK_HZ       10000000U
#define DBY_TOUCH_IC_CLOCK_HZ        16000000U

/* DTBO:10061  spi-mode = <0x00>  → CPOL 0 / CPHA 0.
 * DTBO:10062  reg = <0x00>       → chip select 0 on the SPI bus (the node name
 *             "huawei_thp@10" is a unit-address leftover; it is NOT an I2C
 *             address — an earlier note in evidence/ called it one). */
#define DBY_TOUCH_SPI_MODE           0
#define DBY_TOUCH_SPI_CHIP_SELECT    0

/* DTBO:10063/10064  spi_sync_cs_hi_delay_ns = 200, spi_sync_cs_low_delay_ns = 10.
 * These are the delays THP keeps *around* a transfer while it holds CS. */
#define DBY_TOUCH_CS_HI_DELAY_NS     200U
#define DBY_TOUCH_CS_LO_DELAY_NS     10U

/* ------------------------------------------------------------------ */
/* 1b. Serial-engine registers the census reads and decodes             */
/* ------------------------------------------------------------------ */

/* Until this block existed these four offsets were bare literals inside
 * DbyTouch.cpp, which meant the one part of the driver that touches the
 * transport carried no citation.  The offsets and the decode are the official
 * QcomGeniSpi component's, not this driver's invention:

 *   DOC dby-port/UniXNU-full/drivers/QcomGeniSpi/GeniSpi.h:13   SE_GENI_STATUS
 *   DOC .../GeniSpi.h:15                                        GENI_IF_DISABLE_RO
 *   DOC .../GeniSpi.h:16                                        GENI_FW_REVISION_RO
 *   DOC .../GeniSpi.h:19                                        SE_GENI_DMA_MODE_EN
 *   DOC .../GeniSpi.h:50                                        SE_HW_PARAM_0
 *   DOC .../GeniSpi.c:22-33   geni_spi_read_info(): protocol = (fw >> 8) & 0xff,
 *        if_disable = GENI_IF_DISABLE_RO (& 1 = FIFO interface disabled),
 *        fifo_depth = (hw_param0 & 0x3f0000|0xff0000) >> 16,
 *        fifo_width_bits = (hw_param0 >> 24) & 0x3f.
 *
 * GeniSpi.h:211-212 states the precondition explicitly: these reads are only
 * meaningful "when the SE clocks are already running (the caller must know
 * that)".  This driver cannot create that condition yet — README §12. */
/* GENI output/force-default: written by the official geni_spi_init() (GeniSpi.c:86-87) with
 * GENI_DEFAULT_IO_OUTPUT_CTRL (0x7f) and GENI_FORCE_DEFAULT (1).  Both are safe to write - 0x7f is
 * the documented default of OUTPUT_CTRL - and they double as the write/read-back probe that tells
 * "this window does not respond" apart from "these identity registers legitimately read zero". */
#define DBY_TOUCH_SE_GENI_FORCE_DEFAULT 0x020U   /* DOC GeniSpi.h:10 */
#define DBY_TOUCH_SE_GENI_OUTPUT_CTRL   0x024U   /* DOC GeniSpi.h:11 */
#define DBY_TOUCH_GENI_DEFAULT_OUTPUT_CTRL 0x7FU /* DOC GeniSpi.h:97 */
#define DBY_TOUCH_GENI_FORCE_DEFAULT_BIT   0x1U  /* DOC GeniSpi.h:89 */

#define DBY_TOUCH_SE_GENI_STATUS       0x040U   /* DOC GeniSpi.h:13 */
#define DBY_TOUCH_SE_GENI_IF_DISABLE_RO 0x064U  /* DOC GeniSpi.h:15 */
#define DBY_TOUCH_SE_GENI_FW_REVISION_RO 0x068U /* DOC GeniSpi.h:16 */
#define DBY_TOUCH_SE_GENI_DMA_MODE_EN  0x258U   /* DOC GeniSpi.h:19 */
#define DBY_TOUCH_SE_HW_PARAM_0        0xE24U   /* DOC GeniSpi.h:50 */

/* Field layout of the words above (same source, cited per line). */
#define DBY_TOUCH_SE_FW_PROTO_SHIFT    8U       /* DOC GeniSpi.h:94 */
#define DBY_TOUCH_SE_PROTO_SPI         1U       /* DOC GeniSpi.h:80 */
#define DBY_TOUCH_SE_FIFO_IF_DISABLE   0x1U     /* DOC GeniSpi.h:93 */
#define DBY_TOUCH_SE_FIFO_DEPTH_SHIFT  16U      /* DOC GeniSpi.c:32 */
#define DBY_TOUCH_SE_FIFO_DEPTH_MASK_OLD 0x3F0000U /* DOC GeniSpi.c:31 (QUP < 3.10) */
#define DBY_TOUCH_SE_FIFO_DEPTH_MASK_NEW 0xFF0000U /* DOC GeniSpi.c:31 (QUP >= 3.10) */
#define DBY_TOUCH_SE_FIFO_WIDTH_SHIFT  24U      /* DOC GeniSpi.c:33 */
#define DBY_TOUCH_SE_FIFO_WIDTH_MASK   0x3FU    /* DOC GeniSpi.c:33 */

/* DOC GeniSpi.c:30-31  the mask switch keys off the wrapper's HW version:
 * major = hw >> 28, minor = (hw >> 16) & 0xfff, "new" mask when
 * (major == 3 && minor >= 10) || major > 3.  GeniSpi.c:96/97 also derive the
 * oversampling factor from the same pair, so the field split is not a guess. */
#define DBY_TOUCH_QUPV3_HW_MAJOR_SHIFT 28U      /* DOC GeniSpi.c:30 */
#define DBY_TOUCH_QUPV3_HW_MINOR_SHIFT 16U      /* DOC GeniSpi.c:30 */
#define DBY_TOUCH_QUPV3_HW_MINOR_MASK  0xFFFU   /* DOC GeniSpi.c:30 */
#define DBY_TOUCH_QUPV3_NEW_FIFO_MAJOR 3U       /* DOC GeniSpi.c:31 */
#define DBY_TOUCH_QUPV3_NEW_FIFO_MINOR 10U      /* DOC GeniSpi.c:31 */

/* Comparison values for the census verdict, not hardware facts: an unclocked
 * serial engine returns all-zero or all-ones, and neither is a legal
 * FW_REVISION_RO value for a live engine (DOC GeniSpi.c:22-24 reads that word
 * as the firmware revision).  Nothing else uses these two constants. */
#define DBY_TOUCH_SE_READ_ALL_ZERO     0x00000000U  /* DOC GeniSpi.c:22-24 (sentinel) */
#define DBY_TOUCH_SE_READ_ALL_ONES     0xFFFFFFFFU  /* DOC GeniSpi.c:22-24 (sentinel) */

/* ------------------------------------------------------------------ */
/* 2. Reset / interrupt / chip-select pins (TLMM)                      */
/* ------------------------------------------------------------------ */

/* BASE:10331  pinctrl@f000000 { compatible = "qcom,kona-pinctrl";
 *                               reg = <0xf000000 0x1000000>; }
 * The stock window at 0xF000000 covers 0x0F100000, but the TLMM register file
 * itself starts one megabyte into it.  Two independent sources for that:
 *   - an official same-SoC DTB names the node `pinctrl@f100000`
 *     (dby-port/uefi-dby/.../DbyKeyInputDxe.c:107-113 records exactly this
 *     citation, gts9wifi.dts; the upstream sm8250 pinctrl node is 0x0f100000);
 *   - reading 0x0F000000 returns impossible reset values and the survey loop
 *     stopped mid-print (DbyKeyInputDxe.c:109-111).
 * So 0x0F100000 is sourced, not inferred; the earlier wording in this header
 * called it an inference.  Whether a *write* to a touch pin is retained is a
 * different question and is still measured per pin at stage 2. */
/* The pin controller is split into three tiles on SM8250 and the register for a pin is
 * tile_base + pin * 0x1000, NOT west_base + pin * 0x1000.  mainline pinctrl-sm8250.c carries the
 * tile of every pin (REG_SIZE 0x1000, ctl_reg = REG_SIZE * id, driver indexes regs[g->tile]).
 * The touch pins sit in two different tiles:
 *   gpio 39 (irq), 43 (cs), 40, 41, 42 (spi) -> SOUTH
 *   gpio 98 (reset)                          -> NORTH
 * Reading them through the west base is what produced six 0x00000000 words on 2026-10-31; the one
 * non-zero value ever seen (0x00000001) came from SOUTH + 39 * 0x1000, i.e. the real GPIO_CFG39.
 * The west window stays in the table because west is where the rest of the SoC keeps its pins. */
#define DBY_TOUCH_TLMM_BASE_WEST     0x0F100000ULL
#define DBY_TOUCH_TLMM_BASE_SOUTH    0x0F500000ULL
#define DBY_TOUCH_TLMM_BASE_NORTH    0x0F900000ULL
/* Kept for the source references that still name a single window; the driver picks the tile. */
#define DBY_TOUCH_TLMM_BASE          0x0F100000ULL
#define DBY_TOUCH_TLMM_STRIDE        0x00001000ULL
#define DBY_TOUCH_TLMM_GPIO_CFG      0x0U
#define DBY_TOUCH_TLMM_GPIO_IN       0x4U
#define DBY_TOUCH_TLMM_INTR_CFG      0x8U
#define DBY_TOUCH_TLMM_INTR_STS      0xCU

/* N9Z registers the chip-enable sequence pokes, with the source of each value:
 *   EVENT_BUF 0x2FE00        the vendor's event buffer base.  Three independent alignments in this
 *                            tree already derive it (N9Z_CMD_ADDR 0x2FE50 minus HOST_CMD +0x50,
 *                            N9Z_CMD_STAMP 0x2FE77 minus +0x77, FWINFO at +0x78), and the official
 *                            ShengNovatekTouch driver uses the same +0x50 host-command offset.
 *   FRAME_COUNTER 0x3F8      the only byte range that moves on this board's own frames: the three
 *                            captured dumps all have payload 0x0100..0x0107 = zero, while
 *                            work/verify/diff-focus.py puts the moving bytes at 0x0261,
 *                            0x03F8-0x0403 and 0x0FE5-0x0FFF.  lv=1016 = 0x3F8 was the first
 *                            non-idle payload byte measured by the earlier round, so the counter is
 *                            probed there.  The +0x100 "header peek" layout of the reference driver
 *                            is recorded but is NOT what this part shows, and the row publishes the
 *                            raw bytes so a wrong offset stays visible instead of reading as 0.
 *   N9Z_BOOT_ADDR 0x3F0FE    the vendor's reset/identity word (novatek_thp.c:310-333).
 *   N9Z_IDLE_BYTE 0xAA       brings the chip out of its bootloader into idle; 0x69 does the
 *                            opposite and is what the identify pass sends first. */
#define DBY_TOUCH_EVENT_BUF          0x2FE00U
#define DBY_TOUCH_FRAME_COUNTER      0x3F8U
#define DBY_TOUCH_N9Z_BOOT_ADDR      0x3F0FEU
#define DBY_TOUCH_N9Z_BOOT_BYTE      0x69U
#define DBY_TOUCH_N9Z_IDLE_BYTE      0xAAU
/* Host-command interface at the event buffer.  Source: the official ShengNovatekTouch driver
 * (NvtProtocol.c: EVENT_HOST_CMD = 0x50, nvt_set_custom_cmd, nvt_start_scanning) and this board's
 * own alignment work, which located the event buffer at 0x2FE00 and the command word at +0x50
 * (work/tracks/nvt-header/SPEC.md).  The command is five bytes {0xBF, cmd, 0x00, lo, hi}; the
 * firmware clears byte 0 of it when it has been taken, which is the only acknowledgement.
 * Commands this driver sends: 0x19 = refresh the idle baseline, 0x01 with value 0x0002 = the
 * doze/active-mode setting that makes the chip report. */
#define DBY_TOUCH_EVENT_HOST_CMD     0x50U
#define DBY_TOUCH_HOSTCMD_IDLE       0x19U
#define DBY_TOUCH_HOSTCMD_IDLE_VAL   0x0000U
#define DBY_TOUCH_HOSTCMD_ACTIVE     0x01U
#define DBY_TOUCH_HOSTCMD_ACTIVE_VAL 0x0002U
/* Handshake timings from nvt_set_custom_cmd: a settling delay before the write, then the byte is
 * re-read until it clears, five attempts. */
#define DBY_TOUCH_HOSTCMD_PRE_MS     35U
#define DBY_TOUCH_HOSTCMD_POST_MS    20U
#define DBY_TOUCH_HOSTCMD_TRIES      5U
/* The record the N9Z firmware actually parses, from this board's own vendor kernel
 * (novatek_thp.c:855-913):
 *
 *   latch cmd_addr {FF, hi, mid}                     (touch_driver_write_cmd_addr, :762-788)
 *   one 7-byte transfer:
 *     [0] (cmd_addr & 0x7F) | 0x80
 *     [1] cmd
 *     [2] 0xBB                     handshake byte
 *     [3] arg_lo   [4] arg_hi
 *     [5] stamp - 1                previous firmware stamp, to avoid colliding with the HAL
 *     [6] ~(cmd + 0xBB + arg_lo + arg_hi + stamp) + 1
 *   ack: poll the stamp at CMD_STAMP_ADDR, 5 tries 1 ms apart, accept when it equals what we wrote,
 *        then settle 50 ms.
 *
 * This is NOT the {0xBF, cmd, 0, lo, hi} five-byte record the official NT36532E driver uses; that
 * one has no 0xBB handshake, no stamp and no checksum, so the firmware here does not parse it. */
#define DBY_TOUCH_CMD_HANDSHAKE      0xBBU
#define DBY_TOUCH_CMD_STAMP_ADDR     0x2FE77U
#define DBY_TOUCH_CMD_RETRY_DELAY_MS 1U
#define DBY_TOUCH_CMD_SETTLE_MS      50U
/* A command the vendor kernel itself sends at boot (BT stylus status, disconnect), used to prove the
 * mailbox path end to end before believing anything about the two start-reporting candidates. */
#define DBY_TOUCH_CMD_BT_STATUS      0xC2U
/* Multi-frame sweep: forty snapshots two seconds apart, dumped into the export region so a
 * press/release edge anywhere in an 80-second window is captured inside ONE boot.  Slot size is
 * three export blocks per snapshot, taken backwards from the end of the region. */
#define DBY_TOUCH_FRAME_SNAPSHOTS     40U
#define DBY_TOUCH_FRAME_PERIOD_MS     2000U
/* Live publication period while `sheng-touch-hid=1` is passed.
 *
 * 2026-11-03: 100 ms -> 25 ms.  The 100 ms was a guess made before the frame cost was known; measured
 * on hardware (472 s boot, publishes=3376 over the 390 s after the sweep) the loop ran at 115.5 ms per
 * frame, and since IOSleep is at the top of the loop the SPI frame read itself costs only ~15 ms.
 * So the sample rate was being set by this constant, not by the hardware: 100 ms gave 8.7 Hz, which
 * is too coarse for a drag - a 5 cm/s swipe moves only ~6 mm between samples, and UIKit's gesture
 * recognisers see a series of jumps.  25 ms gives ~40 ms per frame, i.e. ~25 Hz, for a duty cycle of
 * roughly 15/40 of one core.  The same boot page reported LOAD idle=97 %, so that is affordable.
 * This is still polling, not a scan: sheng measured the same polling at 93/300 s of CPU on this
 * silicon (work/tracks/xiaomi-touch/REPORT.md:205-211).  If the next readback shows LOAD sys climbing
 * into double digits, raise this to 40 before touching anything else. */
#define DBY_TOUCH_HID_PERIOD_MS       25U

/* Firmware liveness: after a firmware upload the vendor HAL polls six bytes at chip 0x2FE60 and
 * accepts a first byte in 0xA0..0xAF as "the firmware finished resetting" (nvt_check_fw_reset_state,
 * libafehalW2599Z1100.so:0x2822c, found by work/tracks/thp-daemon-re).  This board's part is a
 * no-flash THP one, so that byte is the cheapest available answer to "is there any firmware running
 * at all" - and the stage-6 frame (8192 bytes of stable uniform random) is what a chip without
 * firmware returns. */

#define DBY_TOUCH_FW_STATE_ADDR      0x2FE60U
#define DBY_TOUCH_FW_STATE_OK_LO     0xA0U
#define DBY_TOUCH_FW_STATE_OK_HI     0xAFU
/* Level of a pad that is muxed to GPIO with OE set: bit 1 of the per-pin IO register at +4, per
 * pinctrl-kona.c:44-55 (in_bit 0, out_bit 1) and pinctrl-msm.c:440-473 (the level goes to io_reg,
 * the output enable to ctl_reg).  The level is NOT in GPIO_CFG - writing only CFG leaves the pad
 * either floating or driving whatever stale bit 1 the IO register already held, which is how the
 * first version of the enable sequence would have held the chip in reset for a whole boot. */
#define DBY_TOUCH_GPIO_OUT_BIT       0x00000002U
#define DBY_TOUCH_GPIO_IN_BIT        0x00000001U

/* GPIO_CFG bitfields.  DOC — this is the upstream pinctrl-sm8250.c PINGROUP()
 * layout, NOT a measurement on this board: pull[1:0] mux[4:2] drv[8:6] oe[9];
 * pull 0b11 = up, mux 0 = GPIO, oe 0 = input.  (Same source used, and the same
 * bit positions exercised, by DbyKeyInputDxe.c:126-134 on GPIO 6 — that write is
 * the observation; the layout itself is documentation.) */
#define DBY_TOUCH_CFG_PULL_MASK      0x00000003U
#define DBY_TOUCH_CFG_PULL_UP        0x00000003U
#define DBY_TOUCH_CFG_PULL_DISABLE   0x00000000U
/* The function field is FOUR bits wide, bits [5:2]: pinctrl-msm.c PINGROUP() builds it as
 * ((func) << 2) with nfuncs = 10 on this SoC, and the official TouchBoard.h writes it the same way
 * ((func & 15) << 2).  This mask was 0x1C (three bits) and would have left a stale bit 5 behind on
 * any read-modify-write that moved a pin between two functions whose difference is bit 2 of the
 * function index.  Found by work/tracks/n9z-enable/REPORT.md §7 against the vendor pinctrl. */
#define DBY_TOUCH_CFG_MUX_MASK       0x0000003CU
#define DBY_TOUCH_CFG_DRV_MASK       0x000001C0U
#define DBY_TOUCH_CFG_OE             0x00000200U
/* drv encoding on kona: (n + 1) * 2 mA, so 0b000 = 2 mA, 0b010 = 6 mA and 0b011 = 8 mA.
 * pinctrl-msm.c:239-242 is the source; the earlier "0b010 = 8 mA" here was wrong and would have
 * written 6 mA wherever the vendor DT says <8>. */
#define DBY_TOUCH_CFG_DRV_8MA        0x000000C0U

/* DTBO:10074  rst_gpio  = <&tlmm 0x62 0x00> → GPIO 98, GPIO_ACTIVE_HIGH
 * DTBO:10075  irq_gpio  = <&tlmm 0x27 0x00> → GPIO 39
 * DTBO:10076  cs_gpio   = <&tlmm 0x2b 0x00> → GPIO 43
 * DTBO:10071  irq_flag  = <0x02>            → INFERENCE: 0x02 is Linux
 *                  IRQ_TYPE_EDGE_FALLING and that is how THP is expected to use
 *                  the property, but nothing on this board has confirmed it.  The
 *                  DT interrupt cell itself carries flags 0 (DTBO:10073), so the
 *                  trigger type comes from this property, not from the irq cell.
 *                  TODO(measure): read INTR_CFG after the IC has been configured
 *                  (stage 2 census prints it) instead of trusting the tag.
 * DTBO:9973-9988   pmx_ts_int_suspend : gpio39, function gpio, drv 2 mA, bias-disable
 * DTBO:9955-9970   pmx_ts_int_active  : gpio39, function gpio, drv 8 mA, bias-pull-up
 * DTBO:9991-10007  pmx_ts_reset_active: gpio98, function gpio, drv 8 mA, bias-disable
 * DTBO:10009-10025 pmx_ts_reset_suspend: gpio98, function gpio, drv 2 mA, bias-disable
 * BASE:15504/15509 qupv3_se14_spi_active: gpio40,41,42,43 function "qup14", 6 mA,
 *                  bias-disable → the CS pad (43) is muxed to the QUP, so CS is
 *                  driven by the serial engine; THP's cs_gpio is the idle-state
 *                  override.  DBY_TOUCH_CS_BY_SERIAL_ENGINE records that reading. */
#define DBY_TOUCH_GPIO_IRQ           39
#define DBY_TOUCH_GPIO_RESET         98
#define DBY_TOUCH_GPIO_CS            43
#define DBY_TOUCH_GPIO_SPI_0         40   /* BASE:15504, assignment per signal TODO(measure) */
#define DBY_TOUCH_GPIO_SPI_1         41
#define DBY_TOUCH_GPIO_SPI_2         42

/* Every pin the touch path touches, with the tile its registers live in.  The value is the index
 * into dbyTouchTlmmBases(); -1 would be a pin this table does not cover. */
#define DBY_TOUCH_TILE_WEST          0
#define DBY_TOUCH_TILE_SOUTH         1
#define DBY_TOUCH_TILE_NORTH         2
/* A pin this table does not cover must not silently land in another tile: 136, for instance, would
 * be south + 0x22000, which is inside a mapped page but belongs to no touch pin. */
#define DBY_TOUCH_TILE_NONE          (-1)
#define DBY_TOUCH_TILE(pin) \
	((((pin) == DBY_TOUCH_GPIO_IRQ) || ((pin) == DBY_TOUCH_GPIO_CS) || \
	  ((pin) == DBY_TOUCH_GPIO_SPI_0) || ((pin) == DBY_TOUCH_GPIO_SPI_1) || \
	  ((pin) == DBY_TOUCH_GPIO_SPI_2)) \
	 ? DBY_TOUCH_TILE_SOUTH \
	 : (((pin) == DBY_TOUCH_GPIO_RESET) ? DBY_TOUCH_TILE_NORTH : DBY_TOUCH_TILE_NONE))
#define DBY_TOUCH_CS_BY_SERIAL_ENGINE 1

/* DTBO:10127  ilitek sub-node overrides rst_gpio to <&tlmm 0x88 0x00> = 136.
 * That pin belongs to the Ilitek variant only.  It is recorded so the static
 * check can prove this driver never uses it. */
#define DBY_TOUCH_ILITEK_RESET_GPIO  136
#define DBY_TOUCH_UNUSED_ILITEK_RESET_GPIO 1

/* DTBO:10065-10070  reset timing, milliseconds (use_mdelay = 1, so these are
 * not microseconds).  "hi" = the time reset is held high before pulling low. */
#define DBY_TOUCH_BOOT_RESET_HI_MS       1U
#define DBY_TOUCH_BOOT_RESET_LOW_MS      0U
#define DBY_TOUCH_BOOT_RESET_AFTER_MS    0U
#define DBY_TOUCH_RESUME_RESET_AFTER_MS  1U
#define DBY_TOUCH_SUSPEND_RESET_AFTER_MS 1U

/* ------------------------------------------------------------------ */
/* 3. Identity                                                         */
/* ------------------------------------------------------------------ */

/* DTBO:10058  compatible = "huawei,thp"  — the platform driver key.  The stock
 * kernel carries the same string in its of_device_id table (see README §1). */
#define DBY_TOUCH_COMPATIBLE         "huawei,thp"

/* DTBO:10085 / DTBO:10120  support_vendor_ic_type = <0x05>.
 * DBY_TOUCH_IC_FAMILY below records what the stock kernel actually builds for
 * this value; see README §1 for the four independent pieces of evidence.
 *
 * Provenance, stated precisely (README §11 re-checked this on 2026-10-29):
 * IC family: Novatek NT36xxx "9Z" (ic_type 5), not Goodix.  Evidence: all eight
 * Huawei dtbo overlays carry only a `novatech` child under huawei_thp@10, the THP core
 * looks the child up by ic_name (goodix's enum stops at 4), and support_vendor_ic_type=5
 * equals N9Z_SERIES.  See work/tracks/touchprot/out/PROTOCOL-SPEC.md.
 *     core is present together with exactly one IC driver, and that driver is
 * IC family: Novatek NT36xxx "9Z" (ic_type 5), not Goodix.  Evidence: all eight
 * Huawei dtbo overlays carry only a `novatech` child under huawei_thp@10, the THP core
 * looks the child up by ic_name (goodix's enum stops at 4), and support_vendor_ic_type=5
 * equals N9Z_SERIES.  See work/tracks/touchprot/out/PROTOCOL-SPEC.md.
 *     `touch_driver_communication_check_for_9966`, `touch_driver_ice_mode_*`,
 *     `goodix_thp`, `goodix_spi_tx_buf`, `[I/THP] %s: ic_type %d`).
 *   - mapping the DT value 0x05 onto that family (0-based index 5 of
 *     `thp_projectid_to_ic_name`) is an INFERENCE, not a measurement.
 *   - the individual part (GT9896 / GT9897 / GT9966) is REQUIRED_MEASURE.
 *     No part number may be written here until `[I/THP] ... ic_type %d` has
 *     been captured from a stock boot or the ICE-mode chip id has been read
 *     over SPI.  README §12 states how. */
#define DBY_TOUCH_VENDOR_IC_TYPE     0x05U   /* DTBO:10085 */
#define DBY_TOUCH_PROJECT_IN_TP      0U    /* DTBO:10119 */
#define DBY_TOUCH_IC_FAMILY       "novatek"
#define DBY_TOUCH_IC_TRANSPORT       "spi"

/* ------------------------------------------------------------------ */
/* 4. Coordinates                                                      */
/* ------------------------------------------------------------------ */

/* DTBO:10101/10102  thp_input_dev { abs_max_x = 0x640; abs_max_y = 0xa00; }
 *                   = 1600 x 2560  → the reported space equals the panel.
 * DTBO:10103        magnification_of_finger_resolution = 0x08
 * DTBO:10108-10110  thp_pen_input_dev { max_x = 0x3200 (12800);
 *                   max_y = 0x5000 (20480); max_pressure = 0x4000 (16384); }
 * porting.toml:125  MEASURED: getevent reports ABS_X 0..12800, ABS_Y 0..20480,
 *                   pressure 0..4096, tilt ±90, BTN_STYLUS — i.e. the *raw*
 *                   space, 8x the panel, despite abs_max_* saying 1600x2560. */
#define DBY_TOUCH_PANEL_W            1600U
#define DBY_TOUCH_PANEL_H            2560U
#define DBY_TOUCH_RAW_W              12800U
#define DBY_TOUCH_RAW_H              20480U
#define DBY_TOUCH_MAGNIFICATION      8U
#define DBY_TOUCH_FINGER_PRESSURE_MAX 4096U
#define DBY_TOUCH_PEN_PRESSURE_MAX   16384U

/* MEASURED getprop.txt:969  ro.panel.hw_orientation = 90
 * MEASURED porting.toml:111 wm size = 1600x2560, density 400.
 * So the native panel is portrait 1600x2560 and Android presents it rotated 90°.
 * Whether XNU must apply the same rotation is decided by whatever presents the
 * framebuffer; keep it as a parameter, never as a hard-coded swap. */
#define DBY_TOUCH_PANEL_ROTATION_DEG 90
#define DBY_TOUCH_ROTATION_IS_RUNTIME 1

/* ------------------------------------------------------------------ */
/* 5. DMA / SMMU                                                       */
/* ------------------------------------------------------------------ */

/* BASE:10143  apps-smmu@15000000 { reg = <0x15000000 0x100000 0x15182000 0x20>;
 *                                  reg-names = "base","tcu-base";
 *                                  qcom,use-3-lvl-tables; }
 * BASE:22142  qupv3_2 wrapper: iommus = <&apps_smmu 0x63 0x00>
 * BASE:22143  qcom,iommu-dma-addr-pool = <0x40000000 0xc0000000>
 * BASE:22144  qcom,iommu-dma = "fastmap"
 *
 * "fastmap" is Qualcomm's identity-mapped domain: the wrapper's DMA master sees
 * physical addresses, no page tables are installed by Linux, and the DMA range
 * must fall inside the pool [0x40000000, 0x100000000).  This is exactly the
 * "apps SMMU bypass path" the unixnu 4K page-table page refers to for touch
 * (docs/zh/xnu/four-k-page-tables.md:158) and the way the UFS driver maps its
 * buffers (docs/zh/drivers/sheng-novatek-touch.md:116).
 *
 * Consequences for this driver (all of them are hard requirements):
 *   - buffers 4 KiB aligned and 4 KiB sized (docs/zh/xnu/four-k-page-tables.md:186)
 *   - physically contiguous, allocated from the pool above
 *   - cached, with explicit clean/invalidate around every transfer
 *   - stream 0x63 must NOT be re-programmed to translate while bypass is live */
#define DBY_TOUCH_APPS_SMMU_BASE     0x15000000ULL   /* BASE:10143 */
#define DBY_TOUCH_SMMU_STREAM_ID     0x63U           /* BASE:22142 */
#define DBY_TOUCH_DMA_POOL_BASE      0x40000000ULL   /* BASE:22143 */
#define DBY_TOUCH_DMA_POOL_SIZE      0xC0000000ULL
#define DBY_TOUCH_DMA_ALIGN          0x1000ULL   /* DOC four-k-page-tables:186 */
#define DBY_TOUCH_SMMU_MODE_BYPASS   1

/* MEASURED dby-port/uefi-dby/.../DbyKeyInputDxe.c:47-49 — writes to the APSS
 * watchdog (0x17410008) and to SMMU (0x15000000) silence/hang this board, while
 * TLMM GPIO_CFG writes are retained.  The touch driver must therefore never
 * write the SMMU base: bypass is inherited state, read-only for us. */
#define DBY_TOUCH_SMMU_WRITE_FORBIDDEN 1

/* ------------------------------------------------------------------ */
/* 6. Stage gate                                                       */
/* ------------------------------------------------------------------ */

/* Mirrors sheng-touch=<n> (docs/zh/drivers/sheng-novatek-touch.md:91-100):
 *   0 = off (default; dby_touch_probe_entry() returns before mapping anything)
 *   1 = read-only census: TLMM pins + SE identity + QUP wrapper version
 *   2 = clock/pin vote (first stage that writes anything)
 *   3 = GENI SPI init + chip identify
 *   4 = reset sequence + frame scan
 *   5 = publish contacts to the snapshot the HID layer reads
 * Parsed from the boot-args key below; unknown keys are ignored by XNU. */
/* DOC docs/zh/drivers/sheng-novatek-touch.md:91 — the stage selector is a boot
 * arg, `sheng-touch=<n>` there; ours uses the same shape under its own key. */
#define DBY_TOUCH_ARG_KEY            "dby-touch"
#define DBY_TOUCH_STAGE_OFF          0
#define DBY_TOUCH_STAGE_CENSUS       1
#define DBY_TOUCH_STAGE_CLOCKS       2
#define DBY_TOUCH_STAGE_IDENTIFY     3
#define DBY_TOUCH_STAGE_FRAMES       4
#define DBY_TOUCH_STAGE_PUBLISH      5
/* 6 = frames plus the vendor's chip-enable sequence (reset release + 0xAA idle) and the two
 * liveness probes: gpio39 edge sampling and the 16-bit frame counter.  A separate stage rather
 * than a new boot argument on purpose - the kernel command line is at 991 of its 1000 bytes, and
 * `dby-touch=<n>` is already parsed. */
#define DBY_TOUCH_STAGE_ENABLE       6
/* 7 = 6 plus the Novatek host-command handshake at the event buffer: {0xBF, cmd, 0x00, lo, hi}
 * written to EVENT_BUF + 0x50.  The vendor KERNEL never sends this - it lives in the closed daemon,
 * which is why the vendor tree cannot answer it - but the official ShengNovatekTouch driver does:
 * nvt_start_scanning() sends cmd 0x19 (idle baseline) then cmd 0x01 (SET ACTIVE MODE), each accepted
 * only when the firmware clears the command byte, retried up to five times.  Stage 7 is the round
 * that asks whether that is what this chip needs after its reset. */
#define DBY_TOUCH_STAGE_HOSTCMD      7
/* 8 = 7 plus the firmware upload.  This part is a no-flash THP one: without the 0x30000-byte image
 * the chip answers identity and register reads but never reports, so nothing above this stage can
 * work until the image is loaded.  Sequence, constants and the image itself:
 * work/tracks/n9z-fw-upload/REPORT.md and package/DbyTouch/DbyTouchFw.h. */
#define DBY_TOUCH_STAGE_FWUPLOAD     8

/* ------------------------------------------------------------------ */
/* 7. Build identity (staleness gate)                                  */
/* ------------------------------------------------------------------ */

/* The build tree keeps the previous round's archive until it is rebuilt, and
 * `-force_load` does not care whether that archive matches this source tree.
 * This string is the one thing that can be read back out of a built image to
 * answer "is this round's code in here?".  Bump it in the same commit as any
 * change that must reach the image, then require it in the product:
 *
 *     llvm-strings <tree>/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/\
 *         mach.development.bcm2837.unstripped | grep 'dby-touch/2026'
 *
 * and on the device, the same string is printed by the census
 * (`DbyTouch: source rev …`).  README §14 step 0 is the full procedure.
 * DOC README §14 (provenance marker, not a hardware value). */
/* -03a: single contact, 25 Hz, axis transpose fixed, Y top-down.
 * -03b: multi-contact reader (up to 10 ids with per-contact lift debounce, row 42 gained c=<count>). */
#define DBY_TOUCH_SOURCE_REV         "dby-touch/2026-11-04-multi"

#endif /* DBY_TOUCH_BOARD_H */
