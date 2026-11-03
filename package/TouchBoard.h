// SPDX-License-Identifier: BSD-2-Clause-Patent
// dby_w09 (SM8250 / kona) touch controller board facts and the pure helpers around them. Every constant states its source.
// Nothing here touches hardware; TouchProbe.cpp owns the MMIO. Derived from UniXNU-sheng package/TouchBoard.h (SM8550) with
// every address replaced by the kona value from the stock kernel tree (/home/wqsqw/kernel).
#ifndef DBY_W09_TOUCH_BOARD_H
#define DBY_W09_TOUCH_BOARD_H
#include <stdint.h>

// Stock DT kona-qupv3.dtsi:787 qupv3_2: qcom,qupv3_2_geni_se@8c0000, reg <0x8c0000 0x2000>.
// Stock DT kona-qupv3.dtsi:963 qupv3_se14_spi: spi@880000, reg <0x880000 0x4000>, qcom,wrapper-core = <&qupv3_2>.
// Our touch panel sits on that engine: DBY W09_VA/huawei_tp.dtsi:80 spi@880000 / huawei_thp@10.
#define TB_QUP_WRAPPER_BASE 0x008c0000ULL
#define TB_SE_BASE 0x00880000ULL
#define TB_SE_SIZE 0x4000ULL
// kona-qupv3.dtsi:792 geniqup wrapper iommus = <&apps_smmu 0x63 0x0>.
#define TB_QUP_SMMU_SID 0x63
// Stock DT msm-arm-smmu-kona.dtsi:71 apps_smmu: apps-smmu@15000000, reg <0x15000000 0x100000>, <0x15182000 0x20>.
#define TB_APPS_SMMU_BASE 0x15000000ULL
#define TB_APPS_SMMU_SIZE 0x100000ULL
// gcc-kona.c: clock-controller@100000 (kona.dtsi). All QUP branches share enable_reg 0x52010 in this SoC; the masks below are
// gcc-kona.c:2985-3205 (.enable_mask / .halt_reg of each branch).
#define TB_GCC_BASE 0x00100000ULL
#define TB_GCC_VOTE_ENABLE 0x52010            // gcc_qupv3_wrap2_* enable_reg
#define TB_GCC_GPLL0_ENABLE 0x52018           // gcc-kona.c:179 gpll0 .enable_reg, mask BIT(0)
#define TB_GCC_GPLL0_MODE 0x0                 // LUCID PLL: .offset 0x0, bit 31 = lock
#define TB_VOTE_WRAP1_CORE_2X (1u<<3)         // gcc_qupv3_wrap2_core_2x_clk, halt 0x23278
#define TB_VOTE_WRAP1_CORE (1u<<0)            // gcc_qupv3_wrap2_core_clk,   halt 0x23270
#define TB_VOTE_WRAP1_M_AHB (1u<<2)           // gcc_qupv3_wrap_2_m_ahb_clk, halt 0x1e004
#define TB_VOTE_WRAP1_S_AHB (1u<<1)           // gcc_qupv3_wrap_2_s_ahb_clk, halt 0x1e008
#define TB_VOTE_WRAP1_S4 (1u<<4)              // gcc_qupv3_wrap2_s0_clk,     halt 0x1e00c (the engine the panel hangs on)
#define TB_VOTE_ALL (TB_VOTE_WRAP1_CORE_2X|TB_VOTE_WRAP1_CORE|TB_VOTE_WRAP1_M_AHB|TB_VOTE_WRAP1_S_AHB|TB_VOTE_WRAP1_S4)
#define TB_HALT_CORE_2X 0x23278
#define TB_HALT_CORE 0x23270
#define TB_HALT_M_AHB 0x1e004
#define TB_HALT_S_AHB 0x1e008
#define TB_HALT_S4 0x1e00c
#define TB_CBCR_CLK_OFF (1u<<31)
// gcc-kona.c:920 gcc_qupv3_wrap2_s0_clk_src: cmd_rcgr 0x1e010, mnd_width 16, hid_width 5, parent_map gcc_parent_map_0,
// DFS-managed (gcc-kona.c:4339 DEFINE_RCG_DFS).
#define TB_S4_RCG 0x1e010
#define TB_RCG_CMD 0x0
#define TB_RCG_CFG 0x4
#define TB_RCG_DFSR_CMD 0x14                  // bit0 DFS enable, bits 4:1 current level
#define TB_RCG_PERF_CFG(l) (0x1c+4*(l))
#define TB_RCG_PERF_M(l) (0x5c+4*(l))
#define TB_RCG_PERF_N(l) (0x9c+4*(l))
#define TB_DFS_LEVELS 8
// Stock DT kona-pinctrl.dtsi:2 tlmm: pinctrl@f000000, reg <0x0F000000 0x1000000>. On kona the pin registers are NOT one flat
// stride from the base: pinctrl-kona.c:26-67 computes ctl_reg = tile_base + REG_SIZE*id, with SOUTH 0x00500000 and NORTH
// 0x00900000, so each pin's offset is looked up below instead of multiplied.
#define TB_TLMM_BASE 0x0f000000ULL
#define TB_GPIO_STRIDE 0x1000
// huawei_tp.dtsi:79-108 plus kona-pinctrl.dtsi:4678 qupv3_se14_spi_active: the four SPI pins are gpio40..43, function
// "qup14", drive-strength 6, bias-disable. The interrupt is gpio39 (irq_gpio), the reset is gpio98 (rst_gpio), CS is gpio43.
#define TB_GPIO_SPI_MISO 40
#define TB_GPIO_SPI_MOSI 41
#define TB_GPIO_SPI_CLK 42
#define TB_GPIO_SPI_CS 43
// huawei_tp.dtsi: cs_gpio = <&tlmm 43 0x00>. That line does NOT mean the board bit-bangs chip select: it feeds
// thp_spi_cs_set(), which is installed as cd->spi_config.pl022_spi_config.cs_control (huawei_thp_core.c:3829, :5426) -
// a PL022 hook the spi-geni path never calls. The stock controller pinctrl muxes all four pads including gpio43 to
// qup14 (kona-pinctrl.dtsi:4677-4691), so on the vendor stack the serial engine generates CS. The probe runs the stock
// form first and the bit-banged form second, and reports both answers (TouchProbe.cpp setCsMode).
#define TB_GPIO_IRQ 39
#define TB_GPIO_RESET 98
// The same node carries a second supplier branch, and that branch resets on a different pad: ilitek{rst_gpio =
// <&tlmm 0x88 0> = 136} (dtbo dump b01.dts:10127) while the parent keeps 98 (:10074). ilitek_thp.c's
// thp_update_rst_gpio() (ilitek_thp.c:606-631) is what makes that swap real, and this board turns it on with
// support_rst_gpio_update = <1> (:10137): it frees the parent's line, requests the child's and drives it high as a
// plain output. So the Ilitek half of this panel is reset by gpio136 and nothing else, and gpio98's level is then
// irrelevant to it.
#define TB_GPIO_RESET_ILITEK 136
// The ilitek branch's own timing and options, all read by thp_parse_timing_config()/thp_parse_feature_ic_config()
// from that node rather than from the parent (ilitek_thp.c touch_driver_init). Values from dtbo dump b01.dts:10126-10137.
#define TB_ILI_RESET_HI_MS 10           // :10130 boot_reset_hi_delay_ms = <0x0a>
#define TB_ILI_RESET_LOW_MS 5           // :10131 boot_reset_low_delay_ms = <0x05>
#define TB_ILI_SPI_HZ 16000000u         // :10129 spi-max-frequency = <0xf42400>, above the parent's 10 MHz (:10059)
#define TB_ILI_SPI_MODE 0               // :10128 spi-mode = <0x00>, the same mode 0 as the parent (:10061)
#define TB_ILI_DETECT_BY_DUMMY_REG 1    // :10134 detect_by_dummy_reg = <0x01>: the ICE dummy register is part of the detect
#define TB_ILI_RST_GPIO_UPDATE 1        // :10137 support_rst_gpio_update = <0x01>: take gpio136, free gpio98
#define TB_ILI_CONTROL_CS_RST 1         // :10135 support_control_cs_rst = <0x01>
// Which supplier adapter this unit is talking to. The stock kernel builds both and lets the one that identifies itself
// keep the bus, so the probe has the same two answers and picks between them at runtime.
#define TB_PROFILE_NOVATEK 0
#define TB_PROFILE_ILITEK 1
#define TB_PULL_UP 3u         // TLMM bias field: bits 1:0 (pinctrl-kona.c:49-54 .mux_bit = 2, .pull_bit = 0,
#define TB_PULL_DOWN 1u       // .drv_bit = 6, .oe_bit = 9): 00 none, 01 down, 10 keeper, 11 up. The bias value therefore
                              // goes in unshifted - it is the mux that starts at bit 2, not the pull.
#define TB_FUNC_QUP1_SE4 1                    // pinctrl-kona.c:1497 funcs[] = {gpio, qup14, ...}: qup14 is slot 1
#define TB_DRIVE_6MA 2                        // drive-strength = <6>: (mA/2)-1
#define TB_SPI_MODE 0                         // huawei_tp.dtsi:89 spi-mode = <0>
#define TB_SPI_DEFAULT_HZ 10000000u           // huawei_tp.dtsi: huawei_thp@10 spi-max-frequency = <10000000>
#define TB_SPI_MAX_HZ 10000000u

// The panel's own coordinate space, straight from this board's devicetree rather than inherited from another board:
// huawei_thp@10/thp_input_dev abs_max_x = <0x640> (1600) and abs_max_y = <0xa00> (2560) - dtbo dump b01.dts:10101-10102.
// That node is the parent of both supplier branches, so the same numbers describe the Novatek and the Ilitek child.
// Both hardware paths report in those units already: ili_report() scales the raw 12-bit sample into them (IliTddi.c:175-176)
// and the N9Z decoder is bounds-checked against them (NvtProtocol.c:503), so nothing between the chip and the consumer needs
// a factor.
#define TB_PANEL_ABS_MAX_X 1600
#define TB_PANEL_ABS_MAX_Y 2560
// The devicetree spans describe the digitizer's own frame, which is portrait: 1600 across, 2560 down. The UI this
// board runs is the other way round - 2560 wide by 1600 tall, the shape the panel is physically mounted in - so the
// published axes are the UI's, not the devicetree's. Everything below the driver (NativeHidInput's scaling, the
// scripted swipes' percentages) then speaks one coordinate space: x across the landscape screen, y down it.
#define TB_UI_ABS_MAX_X 2560
#define TB_UI_ABS_MAX_Y 1600
// The published space is the panel's own frame, in the devicetree's own units, with no rotation and no rescaling. Axis x is
// the axis the framebuffer scans out as its width, and the two frames really are the same shape: this board's scan-out is
// 1600 columns by 2560 rows - scripts/patch-xnu-entry-probe.py:76-79 and scripts/xnu-panic-screen.c:238-241, both written
// against the device's own vc_info, which is also why the console has to rotate into a landscape canvas to be readable -
// and the digitizer's axes are 1600 by 2560 (b01.dts:10101-10102 above). Those spans only pair up without a transpose, so
// the transpose this file used to do (short axis "becomes the UI's y", long axis "becomes the UI's x", a landscape
// convention inherited from the sheng board, whose GOP is 2560x1600) rotated every contact through 90 degrees and stretched
// the aspect by 2560/1600 against the panel it was supposed to match.
#define TB_X_AXIS_MAX TB_UI_ABS_MAX_X
#define TB_Y_AXIS_MAX TB_UI_ABS_MAX_Y
// Which way each axis runs along its span has not been observed on this board: a photograph cannot show it, and the
// devicetree states only the spans. sheng publishes Y mirrored for its panel; until a swipe on this one is followed on
// screen, the raw value is published and TB_Y_MIRROR selects the flip if the observation ever says otherwise.
#define TB_Y_MIRROR 0
// The ruler the HID consumer normalises against. The driver that is actually linked into this kernel
// (drivers/ShengHidInput/NativeHidInput.cpp, touchNotify) does
//   x = transformCoordinate(xs[i], 30480), y = transformCoordinate(ys[i], 20320)
// with transformCoordinate(value, maximum) = value * 32767 / (maximum - 1) after clamping value to [0, maximum).
// Those two maxima are hard-coded to that board's touch-core axis maxima (3048 px long, 2032 px short, in 0.1 px), so
// a publisher whose own span is 2560x1600 px has to scale into this ruler or every contact lands in a fraction of the
// screen: at 25600/16000 published raw, a finger at the far corner only reached 84% of x; divided down to panel px it
// reaches 8.4%. pipa does exactly this conversion on its publish side (TouchProbe.cpp:318, p[i].x * 30480u / xmax with
// xmax from the firmware info block), because the consumer is a shared kit and is not edited per board.
// Axis 0 of this ruler is the long axis of the panel (the touch core's x, and the point report's y).
#define TB_HID_RULER_LONG 30480
#define TB_HID_RULER_SHORT 20320

static inline uint32_t tb_pin_cfg(unsigned func, unsigned drive_code) {return (func&15u)<<2|(drive_code&7u)<<6;}   // pull disabled
// ctl_reg of a pin, from pinctrl-kona.c PINGROUP(): SOUTH 0x00500000 + 0x1000*id, NORTH 0x00900000 + 0x1000*id.
static inline uint32_t tb_tlmm_ctl(unsigned gpio) {
    if (gpio >= 40 && gpio <= 43) return 0x00500000u + 0x1000u * gpio;   // SOUTH (qup14 group)
    switch (gpio) {
        case 39: return 0x00500000u + 0x1000u * 39;                      // SOUTH, ts_int
        case 98: return 0x00900000u + 0x1000u * 98;                      // NORTH, ts_reset
        case 136: return 0x00100000u + 0x1000u * 136;                     // WEST, ts_reset of the other panel supplier
        default: return 0x00500000u + 0x1000u * gpio;
    }
}
static inline uint32_t tb_tlmm_io(unsigned gpio) {return tb_tlmm_ctl(gpio)+4;}
// A plain GPIO output: function slot 0 (gpio), the drive code, and the output-enable bit (pinctrl-msm packs OE at bit 9).
// Used to drive the panel's reset line, which the stock driver holds high (nt36xxx.c:810).
#define TB_FUNC_GPIO 0
#define TB_OE_BIT 9
// The tlmm io register keeps the pad level in bit 0 (in_bit) and the output latch in bit 1 (out_bit) - pinctrl-kona.c:55-56.
// Bit 0 is the read-only input, so writing 1 there leaves the latch at 0 and the pad keeps driving low: a high is 2.
#define TB_PIN_HIGH 2u
#define TB_PIN_LOW 0u
static inline uint32_t tb_gpio_out_cfg(unsigned drive_code) {return (TB_FUNC_GPIO<<2)|((drive_code&7u)<<6)|(1u<<TB_OE_BIT);}

// Source clock rates by RCG source-select value (gcc-kona.c:53 gcc_parent_map_0: 0 = TCXO, 1 = GPLL0 main, 6 = GPLL0 even).
// GPLL0 is 600 MHz and even 300 MHz, from the frequency table (100 MHz = main / 6, 75 MHz = even / 4).
static inline uint64_t tb_src_hz(unsigned sel) {return sel==0?19200000ULL:sel==1?600000000ULL:sel==6?300000000ULL:0;}
// clk-rcg2.c calc_rate: half-integer divider, then M/N.
static inline uint64_t tb_calc_rate(uint64_t prate, uint32_t m, uint32_t n, unsigned mode, unsigned hid) {
    if (hid) {prate*=2;prate/=hid+1;}
    if (mode && n) {prate*=m;prate/=n;}
    return prate;
}
// Frequency of DFS level l from its CFG (src 10:8, mode 13:12, hid 4:0 on this 5-bit RCG), M register and N register
// (stored inverted: n = (~N & mask) + m, mnd_width 16).
static inline uint64_t tb_dfs_level_hz(uint32_t cfg, uint32_t mreg, uint32_t nreg) {
    unsigned src=(cfg>>8)&7,mode=(cfg>>12)&3,hid=cfg&0x1f;
    uint32_t m=0,n=0;
    if (mode) {m=mreg&0xffff;n=((~nreg)&0xffff)+m;}
    return tb_calc_rate(tb_src_hz(src),m,n,mode,hid);
}
struct tb_clock_choice {unsigned level, div;uint64_t sclk_hz, actual_hz;};
// Choose the DFS level that gives the highest SPI rate not above target with divider div = ceil(sclk / (os * target)).
// Returns 0 on success.
static inline int tb_choose_clock(const uint64_t *level_hz, unsigned n, unsigned os, unsigned target, struct tb_clock_choice *out) {
    int found=0;
    for (unsigned l=0;l<n;++l) {
        uint64_t f=level_hz[l];if (!f) continue;
        uint64_t d=((f+(uint64_t)os*target-1)/((uint64_t)os*target));if (d<1) d=1;if (d>0xfff) continue;
        uint64_t actual=f/(os*d);if (actual>target) continue;
        if (!found || actual>out->actual_hz || (actual==out->actual_hz && d<out->div)) {
            out->level=l;out->div=(unsigned)d;out->sclk_hz=f;out->actual_hz=actual;found=1;
        }
    }
    return found?0:-1;
}
#endif
