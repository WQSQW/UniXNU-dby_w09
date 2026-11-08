/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* 四颗 smartpa：三套 BOM、每颗一份寄存器表、复位电平按 BOM 各自的 ctl_sequence。
 *
 * 硬件（本机厂商内核 4.19 的设备树与出厂包实证，不是推测）：
 *   arch/arm64/boot/dts/vendor/qcom/kona/DBY/W09_VA/huawei-audio.dtsi —— 十二个 smartpa 节点挂在同一个
 *   &qupv3_se3_i2c（qup3_se3_core 在 0x98c000）上，按芯片分三套，每套四颗（左上/右上/左下/右下）：
 *     0x30..0x33 = chip_model "06-01" = aw88262，reg_bits=<8>  val_bits=<16>，version_regs=<0x00 1 0>，
 *                  ctl_sequence=<0 1 1 1>            —— 末端 state=1，复位放开在高
 *     0x34..0x37 = chip_model "01-02" = tfa9874，同上 8/16，version_regs=<0x03 1 0>，
 *                  ctl_sequence=<1 1 0 1>            —— 末端 state=0，放开在低
 *     0x50..0x53 = chip_model "03-01" = cs35l36，reg_bits=<32> val_bits=<32>，max_register=0xe0ffff，
 *                  version_regs=<0x0 1 0>，ctl_sequence=<1 1 0 1>
 *   三套共用同一根复位：gpio_reset = <&tlmm 109 0>（flag 0 = 高有效）。
 *
 *   ctl_sequence 的排法是 <state delay state delay>：smartpakit_i2c_ops.c:1645 按
 *   sizeof(struct smartpakit_reg_ctl)/sizeof(unsigned) = 3 取模，但 version_regs 用的是同一三元组；
 *   gpio 序列由 smartpakit_i2c_parse_dt_reset 读成 <state delay> 对，smartpakit_i2c_hw_reset_gpio_ctl()
 *   （同文件 498-533）从头到尾依次 gpio_direction_output(state) + smartpakit_delay(delay)，
 *   而 478 行的“正常态”判据取的就是 node[num-1].state —— 也就是序列最后那个电平。
 *   上一版把这根线一律拉高：对 aw88262 是对的，对另外两套正好相反，芯片一直被按在复位里，
 *   所以那八次全是 ADDR_NACK。这一版按 BOM 各自的序列跑，另一头再反过来跑一次。
 *
 * 固件（extract_pa_tables.py 从出厂包 odm/etc/audio/{awinic,nxp,cs} 三个目录的 _configs.xml 原样抽出）：
 *   窄表 struct dby_pa_reg（8 位地址 + 16 位值 + 延时），宽表 struct dby_pa_reg32（32 + 32）。
 *   保留 INIT_CHIP / POWER_ON / POWER_OFF / MMI_POWER_OFF 四个场景，共 48 张。
 *
 * 怎么算“这颗在”（判据抄驱动，不抄魔数）：
 *   smartpakit.c:2873-2884 的存活检查 = 读 version_regs_seq->regs[0].addr 一个寄存器，
 *   ret<0 或 value==0 就判失败，从来没有比对过 0x1852 / 0x74 这种常量。
 *   所以这里也用“读得到且非零”，魔数只在日志里带上做参照：
 *     aw882xx_reg.h:5 AW882XX_ID_REG=0x00、aw882xx.h:67 AW882XX_ID=0x1852
 *     tfa98xx.c:4350  revision 寄存器 0x03，低字节 0x74 = tfa9874
 *     cs35l36 的 version_regs 是 0x0（设备 ID 在 0x1），这里只按非零判
 *
 * 传输格式 = regmap-i2c（smartpakit_i2c_ops.c:2106 regmap_init_i2c + dts 的 reg_bits/val_bits）：
 *   一次写 = 地址字节高位在前 + 值字节高位在前；一次读 = 先写地址（保持 stop，重复起始），再读 val_bits/8 字节。
 *   cs35l36 的写是 4+4=8 字节，正好在 geni_i2c_start_write_bytes 的上限内（GeniI2c.c:61 允许 1..8），
 *   读是 4 字节地址 + 4 字节值，也在 geni_i2c_start_read_bytes 的两个上限内（同文件 69 允许各 1..4）。
 *
 * 顺序：dby-pa=0/1/2 指定先试哪套（aw/nxp/cs），其余值按 dts 排的顺序全试；每套先按自己的序列放开复位再探，
 *   无人应答就把序列整个反相再探一次。三套 × 两种电平 = 6 轮，每轮 200 ms 稳定 + 四笔探测（每笔上限 10 ms），
 *   全空最坏约 1.5 s；命中那套还要跑一遍 INIT_CHIP。这一层跟在点屏序列里同步跑，这就是它的上限。
 *
 * 这一层只做到「认出是哪套、把四颗各自的 INIT_CHIP 跑完」。POWER_ON 要等 TDM 有数据再开，
 * 否则只是把 PA 推到放大态听杂音。播放通路这一侧不可达，原因不是“机子里没有 ADSP 固件”：本机
 * modem.img 的 FAT 就带着 adsp.mdt + adsp.b00..b18（22 个文件已抽到 D:\Linux\stock\adsp_unit 核对过），
 * 缺的是 XNU 这边的 PIL-TZ 装载器和 GPR/AudioReach 那一半——&smartpakit algo_in=<1> 走 soc_dsp、
 * &hw_adsp_misc、&wsa_spkr_en1 在 dtsi 里也是 disabled。下面那段 [pa-adsp] 只读调查就是这一步的第一档：
 * 先量清楚启动阶段到底留下了什么，再决定要不要写装载器。
 */
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include "GeniI2c.h"
#include "DbyPaTables.h"

namespace {
constexpr uint64_t kGcc = 0x100000, kSe = 0x98c000;
// gcc-kona.c 里 wrap0 的五个分支，全部经 0x52008 投票：
//   m_ahb bit 6 (halt 0x17004)、s_ahb bit 7 (0x17008)、core bit 8 (0x23000)、
//   core_2x bit 9 (0x23008)、s3 bit 13 (0x1739c)。
// 和背光那套（wrap1，bit 18/19/20/21/25）不是同一组位，别混。
constexpr uint32_t kGccVote = 0x52008;
constexpr uint32_t kVoteBits = (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 13);
constexpr uint32_t kHalt[] = {0x17004, 0x17008, 0x23000, 0x23008, 0x1739c};

/* TLMM。kona-pinctrl.dtsi:2 写的是 pinctrl@f000000，NORTH 段的基址偏移 0x900000（pinctrl-kona.c:23），
 * 所以 GPIO109/119/120 都落在 0x0F900000 起的这一段里；这一段 1 MiB 覆盖到 gpio255 的 ctl，够用。
 * 寄存器布局也来自 pinctrl-kona.c:26 的 PINGROUP：ctl 在 base+0x1000*id，io 在它 +4（老的 msm8953 那套是反的）；
 * mux_bit=2 且 nfuncs=10，所以功能选择占 bits[5:2]、写的是 funcs[] 的下标而不是 kona_functions[] 的全局编号
 * （qup3 在全表里排 66，但在每个 pin 的 funcs[] 里是 [1]，写进去的值是 1）。 */
constexpr uint64_t kTlmmNorth = 0x0F900000;
constexpr uint32_t kResetGpio = 109;             // 三套 BOM 共用这一根，dtsi 的 gpio_reset
constexpr uint32_t kSclGpio = 119, kSdaGpio = 120;
constexpr uint32_t kPinCtl(uint32_t id) {return id * 0x1000u;}
constexpr uint32_t kPinPullBits = 0x003u;        // pull_bit=0，两位：0=no pull
constexpr uint32_t kPinMuxBits = 0x03cu;         // mux_bit=2，四位
constexpr uint32_t kPinMuxQuP3 = 1u << 2;        // funcs[1] = qup3
constexpr uint32_t kPinDriveBits = 0x1c0u;       // drv_bit=6，三位；dts 的 drive-strength=2mA 对应 0
constexpr uint32_t kPinOutputEnable = 1u << 9;
constexpr uint32_t kPinOutBit = 1u << 1;         // io_reg 的 out_bit=1

/* SE 寄存器：GeniI2c.h 里没有这两个，时序在 prepare 之前就得写好（prepare 会拿 SCL_COUNTERS 当“时钟已配”的判据）。 */
constexpr uint32_t kRegIfDisable = 0x64u;   // SE_GENI_DISABLE
constexpr uint32_t kRegClkSel = 0x7cu;      // SE_GENI_CLK_SEL（geni-se.h:94，CLK_SEL 占 bits[2:0]）
/* 主线 i2c-qcom-geni.c:191 的 geni_i2c_clk_map_19p2mhz 第一行：100 kHz 用
 * {clk_div, t_high, t_low, t_cycle} = {7, 10, 12, 26}，同一文件 185-188 给的算式是
 * SCL 周期 = t_cycle_cnt * clk_div / source_clock —— 26*7/19.2 MHz = 9.5 µs，即 105 kHz。 */
constexpr uint32_t kSeClkDiv = 7, kSeTHigh = 10, kSeTLow = 12, kSeTCycle = 26;

/* SE3 自己的频率源（RCG）。0x52008 那五个位只放开分路；s3 分路的父节点是
 * gcc_qupv3_wrap0_s3_clk_src —— 厂商 gcc-kona.c:2761 里这个 clk_branch 的 halt_reg 是 0x1739c，
 * 而同一个结构体后面紧跟着的就是它的 RCG（cmd_rcgr = 0x173a0，cfg 在 +4）。
 * 放开分路不会让父节点自己起来；而父节点死了的时候分路的 CLK_OFF 也读 0。这条四步写法来自
 * 朋友在真机跑通的 DbyTouchBoard.h:52-79，上一版按它起根时钟后量到 cfg/cmd 正常，根时钟已证好。 */
constexpr uint32_t kSe3RcgCmd = 0x173a0, kSe3RcgCfg = 0x173a4;
constexpr uint32_t kRcgUpdate = 1u << 0, kRcgRootEn = 1u << 1, kRcgRootOff = 1u << 31;
constexpr uint32_t kRcgCfgCxo19M = 0x00100001u;  // SRC_SEL=0（CXO 19.2 MHz）、DIV=1、HW_CLK_CTRL(BIT20)
constexpr uint32_t kRcgCfgMask = 0x001037ffu;    // 只比 DIV/SRC_SEL/MODE/HW_CLK_CTRL 这几位

/* 三套 BOM。字段全部来自 huawei-audio.dtsi 的对应节点，magic 只用于日志（判据是非零应答）。
 * bank 是窄表索引表里的行号（0=aw88262、1=tfa9874），宽表那一套用不到它。 */
struct PaSet {
    const char *name;
    uint8_t base;             // 第一颗的 7 位地址
    uint8_t regBytes;         // reg_bits/8
    uint8_t valBytes;         // val_bits/8
    uint8_t bank;             // kDbyPaBank 的行号，wide 的那套不用
    uint32_t idReg;           // version_regs 的第一个字段
    uint32_t magic;           // 参照值，0 表示厂商表里没有这个常量
    bool wide;                // true -> 用 kDbyPaBank32
    uint8_t seq[4];           // ctl_sequence：<state delay state delay>，单位 ms
};

const PaSet kSets[3] = {
    {"aw88262", 0x30, 1, 2, 0, 0x00000000u, 0x1852u, false, {0, 1, 1, 1}},
    {"tfa9874", 0x34, 1, 2, 1, 0x00000003u, 0x0074u, false, {1, 1, 0, 1}},
    {"cs35l36", 0x50, 4, 4, 0, 0x00000000u, 0u, true, {1, 1, 0, 1}},
};
constexpr unsigned kSetCount = sizeof kSets / sizeof kSets[0];
constexpr unsigned kPAs = 4;
constexpr uint32_t kPollUs = 250, kPollTries = 40;   // 每笔传输最多等 10 ms

struct Window {
    IOMemoryMap *map;
    volatile uint32_t *va;
};

Window gGcc, gSe, gTlmm;
GeniI2c gBus;
bool gBusReady;
uint32_t gTransfers, gErrors;
bool gRcgOk;
uint32_t gRcgCmdPre, gRcgCmd, gRcgCfg, gRcgCfgPre;
uint32_t gErrClass, gErrStatus;
/* probe 命中那一套的身份留到播放时用：厂商的开声不是跟着 INIT 一起做的，
 * 见 runBankScene 上方的原文出处。 */
const PaSet *gPaSet;
uint32_t gPaFound;

bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}

uint32_t rd(volatile uint32_t *base, uint32_t reg) {return base[reg / 4];}

void wr(volatile uint32_t *base, uint32_t reg, uint32_t value) {
    base[reg / 4] = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

uint32_t ioRead(void *ctx, uint32_t reg) {return rd(static_cast<volatile uint32_t *>(ctx), reg);}
void ioWrite(void *ctx, uint32_t reg, uint32_t value) {wr(static_cast<volatile uint32_t *>(ctx), reg, value);}

/* 投票并等分支退出 halt。AHB 分支跑在硬件自门控下（HW_CTL，分支寄存器 bit 1）：总线空闲时它的 CLK_OFF 位就说 off，
 * 照 clk-branch.c 在该模式下的做法不等它——背光那次 (int-231) 就是在 wrap1 s_ahb 上等死的。 */
bool clocksOn() {
    if (!gGcc.va) return false;
    wr(gGcc.va, kGccVote, rd(gGcc.va, kGccVote) | kVoteBits);
    for (uint32_t halt : kHalt) {
        if (rd(gGcc.va, halt) & (1u << 1)) continue;
        unsigned us = 0;
        while ((rd(gGcc.va, halt) & (1u << 31)) && us < 200) {IODelay(1); ++us;}
        if (us >= 200) {kprintf("[pa] halt 0x%05x stuck\n", halt); return false;}
    }
    return true;
}

/* 起 SE3 的根时钟。四步顺序固定：先钉住根（ROOT_EN），再写配置，再 UPDATE（自清位），最后放开 ROOT_EN。
 * 读回 0xFFFFFFFF 的 GCC 不能读改写。等的条件是 ROOT_OFF 清掉且 UPDATE 自清，且必须在放开 ROOT_EN 之前等。 */
bool startSe3Root() {
    if (!gGcc.va) return false;
    gRcgCmdPre = rd(gGcc.va, kSe3RcgCmd);
    gRcgCfgPre = rd(gGcc.va, kSe3RcgCfg);
    if (gRcgCmdPre == 0xffffffffu) {kprintf("[pa] gcc not readable\n"); return false;}
    wr(gGcc.va, kSe3RcgCmd, gRcgCmdPre | kRcgRootEn);
    wr(gGcc.va, kSe3RcgCfg, kRcgCfgCxo19M);
    wr(gGcc.va, kSe3RcgCmd, gRcgCmdPre | kRcgRootEn | kRcgUpdate);
    for (unsigned i = 0; i < 50; ++i) {
        const uint32_t cmd = rd(gGcc.va, kSe3RcgCmd);
        if (!(cmd & kRcgRootOff) && !(cmd & kRcgUpdate)) {gRcgOk = true; break;}
        IOSleep(1);
    }
    wr(gGcc.va, kSe3RcgCmd, rd(gGcc.va, kSe3RcgCmd) & ~kRcgRootEn);
    gRcgCmd = rd(gGcc.va, kSe3RcgCmd);
    gRcgCfg = rd(gGcc.va, kSe3RcgCfg);
    kprintf("[pa] rcg pre cfg=0x%08x cmd=0x%08x -> cfg=0x%08x cmd=0x%08x ok=%d s3halt=0x%08x\n",
            gRcgCfgPre, gRcgCmdPre, gRcgCfg & kRcgCfgMask, gRcgCmd, gRcgOk, rd(gGcc.va, 0x1739cu));
    return gRcgOk;
}

/* 这一版只把引脚接上去：109 设成输出但不预设电平（上一版在这里一律拉高，正好把另外两套按在复位里），
 * 末端电平交给 resetPa 按 BOM 的 ctl_sequence 决定；119/120 复用成 qup3。 */
bool configurePins() {
    if (!gTlmm.va) return false;
    wr(gTlmm.va, kPinCtl(kResetGpio),
       (rd(gTlmm.va, kPinCtl(kResetGpio)) & ~(kPinPullBits | kPinMuxBits | kPinDriveBits)) | kPinOutputEnable);
    const uint32_t i2cPins[] = {kPinCtl(kSclGpio), kPinCtl(kSdaGpio)};
    for (unsigned i = 0; i < sizeof i2cPins / sizeof i2cPins[0]; ++i)
        wr(gTlmm.va, i2cPins[i],
           (rd(gTlmm.va, i2cPins[i]) & ~(kPinPullBits | kPinMuxBits | kPinDriveBits)) | kPinMuxQuP3);
    kprintf("[pa] pins %u ctl=0x%08x io=0x%08x scl ctl=0x%08x sda ctl=0x%08x\n", kResetGpio,
            rd(gTlmm.va, kPinCtl(kResetGpio)), rd(gTlmm.va, kPinCtl(kResetGpio) + 4),
            rd(gTlmm.va, kPinCtl(kSclGpio)), rd(gTlmm.va, kPinCtl(kSdaGpio)));
    return true;
}

/* 照 dts 的 ctl_sequence 走一遍复位：两对 <state delay ms>，依次写 io_reg 的 out_bit。
 * invert 非零把两个 state 都取反——那正好是另一套 BOM 的末端电平，用来在没人应答时排除“电平反了”。
 * 放开之后等 200 ms：这是同一个节点自己写的 reset_debounce_wait_time = <200>（huawei-audio.dtsi:149），
 * 不是拍的。tfa9874/cs35l36 这类带内核的功放复位放开后要先从自己的 ROM 起来，那段时间 I2C 上就是
 * 不应答——上一版只等了几毫秒，把“还没 boot 完”读成了“没有这颗”。 */
void resetPa(const PaSet &set, int invert) {
    if (!gTlmm.va) return;
    const uint32_t io = kPinCtl(kResetGpio) + 4;
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t state = set.seq[i * 2] ^ (invert ? 1u : 0u);
        const uint32_t delay = set.seq[i * 2 + 1];
        wr(gTlmm.va, io, state ? kPinOutBit : 0u);
        if (delay) IOSleep(delay);
        else IODelay(1000);
    }
    IOSleep(200);
}

/* 把 SE 的 I2C 时序写进去。bootloader 没碰过 se3，所以 CLK_CFG 的 clk_div 和 SCL_COUNTERS 都是空的——
 * geni_i2c_prepare 第一句就拿 SCL_COUNTERS 非零当时钟已配的证据。接口先关掉再配，配完放开。 */
void programSe3Timing() {
    wr(gSe.va, kRegIfDisable, 1u);
    wr(gSe.va, kRegClkSel, 0);
    wr(gSe.va, 0x48u, (rd(gSe.va, 0x48u) & ~0xf0u) | (kSeClkDiv << 4) | 1u);
    wr(gSe.va, GENI_SCL_COUNTERS, (kSeTHigh << 20) | (kSeTLow << 10) | kSeTCycle);
    wr(gSe.va, kRegIfDisable, 0u);
}

/* 一笔传输，轮询到完。返回 0 完成、-1 被拒、-2 总线错、-3 超时。
 * 三种“失败”分开算：-1 引擎还停在上一条命令里，-2 从机不应答或总线电气问题（error_status 里 bit9 起
 * 那几位就是 NACK/仲裁丢失），-3 引擎一动没动（时钟不在）。 */
int xfer(const uint8_t *write, uint32_t writeLen, uint32_t readLen) {
    if (!gBusReady) return -1;
    const int rc = readLen ? geni_i2c_start_read_bytes(&gBus, write, writeLen, readLen)
                           : geni_i2c_start_write_bytes(&gBus, write, writeLen);
    if (rc) {gErrClass = GeniErrBusy; ++gErrors; return -1;}
    for (uint32_t i = 0; i < kPollTries; ++i) {
        const int state = geni_i2c_poll(&gBus);
        if (state == GeniDone) {++gTransfers; return 0;}
        if (state == GeniFailed) {gErrClass = gBus.error; gErrStatus = gBus.error_status; ++gErrors; return -2;}
        IODelay(kPollUs);
    }
    geni_i2c_timeout(&gBus);
    gErrClass = GeniErrTimeout;
    gErrStatus = gBus.error_status;
    ++gErrors;
    return -3;
}

bool selectAddress(uint8_t address) {
    if (gBus.address == address) return true;
    if (geni_i2c_busy(&gBus)) return false;
    gBus.address = address;
    return true;
}

/* regmap-i2c 的窄/宽两种帧：地址字节高位在前，值字节高位在前。cs35l36 是 4+4=8，正好在上限内。 */
bool writeReg(const PaSet &set, uint8_t address, uint32_t reg, uint32_t value) {
    if (!selectAddress(address)) return false;
    uint8_t bytes[8];
    unsigned n = 0;
    for (unsigned i = set.regBytes; i-- > 0;) bytes[n++] = static_cast<uint8_t>(reg >> (8u * i));
    for (unsigned i = set.valBytes; i-- > 0;) bytes[n++] = static_cast<uint8_t>(value >> (8u * i));
    return xfer(bytes, n, 0) == 0;
}

/* 读回来的是大端：线上第一个字节落在 rx 的 bits 7:0（GeniI2c.c:16 的打包方式 + :123 的一次 FIFO 读），
 * 所以第 i 个字节就是高位往后的第 i 个。 */
bool readReg(const PaSet &set, uint8_t address, uint32_t reg, uint32_t *value) {
    if (!selectAddress(address)) return false;
    uint8_t bytes[4];
    unsigned n = 0;
    for (unsigned i = set.regBytes; i-- > 0;) bytes[n++] = static_cast<uint8_t>(reg >> (8u * i));
    if (n > 4 || xfer(bytes, n, set.valBytes) != 0) return false;
    uint32_t v = 0;
    for (unsigned i = 0; i < set.valBytes; ++i) v = (v << 8) | ((gBus.rx >> (8u * i)) & 0xffu);
    *value = v;
    return true;
}

/* 一张窄表跑完。厂商表里只有三类：掩码 0 = 纯延时，0xFFFF = 直接写，其余 = 读改写。 */
void runScene(const PaSet &set, uint8_t address, const dby_pa_scene &scene) {
    for (unsigned i = 0; i < scene.count; ++i) {
        const dby_pa_reg &r = scene.regs[i];
        if (r.delay_ms) IOSleep(r.delay_ms);
        if (r.mask == 0x0000) continue;
        if (r.mask == 0xFFFF) {writeReg(set, address, r.addr, r.val); continue;}
        uint32_t current = 0;
        if (!readReg(set, address, r.addr, &current)) continue;
        writeReg(set, address, r.addr, (current & ~r.mask) | (r.val & r.mask));
    }
}

/* 宽表同款，掩码全 1 才算直接写。 */
void runScene32(const PaSet &set, uint8_t address, const dby_pa_scene32 &scene) {
    for (unsigned i = 0; i < scene.count; ++i) {
        const dby_pa_reg32 &r = scene.regs[i];
        if (r.delay_ms) IOSleep(r.delay_ms);
        if (r.mask == 0u) continue;
        if (r.mask == 0xffffffffu) {writeReg(set, address, r.addr, r.val); continue;}
        uint32_t current = 0;
        if (!readReg(set, address, r.addr, &current)) continue;
        writeReg(set, address, r.addr, (current & ~r.mask) | (r.val & r.mask));
    }
}

/* 一套的四颗读一遍 version 寄存器，四颗一起只出一个判词，不刷屏。
 * 判据抄 smartpakit.c:2873-2884：读得到且非零才算在。*ids 带回首末两颗的原始值，
 * “NACK”（rc 非 0）和“应答但读出 0”（rc 为 0、id 为 0）是两种完全不同的故障。 */
uint32_t probeSet(const PaSet &set, uint32_t *ids, uint32_t *rcs) {
    uint32_t found = 0;
    for (unsigned i = 0; i < kPAs; ++i) {
        uint32_t id = 0;
        const bool ok = readReg(set, static_cast<uint8_t>(set.base + i), set.idReg, &id);
        ids[i] = id;
        rcs[i] = ok ? 0u : (gErrClass == GeniErrBus ? 2u : (gErrClass == GeniErrTimeout ? 3u : 1u));
        if (ok && id != 0) found |= 1u << i;
    }
    return found;
}

/* 一张场景表跑四颗。play 为假是 INIT，为真是工厂包里的 on 场景。厂商的功放开声不在 INIT 里：
 * tfa98xx.c:2551-2579 在全部 device start 之后逐颗 tfa_dev_set_state(TFA_STATE_UNMUTE)，经
 * tfa_dsp.c:3954-3955 的 tfa98xx_set_mute(Tfa98xx_Mute_Off) 落到 tfa_dsp.c:1015-1069，写的正是
 * 寄存器 0x00 的 AMPE 与 DCA；而 INIT 表只把 0x00 写成 0x0002，功放和升压都还是关着的。 */
unsigned runBankScene(const PaSet &set, uint32_t found, bool play) {
    unsigned ready = 0;
    for (unsigned i = 0; i < kPAs; ++i) {
        if (!(found & (1u << i))) continue;
        if (set.wide) {
            const dby_pa_bank32 &bank = kDbyPaBank32[i];
            const dby_pa_scene32 &scene = play ? bank.on : bank.init;
            runScene32(set, static_cast<uint8_t>(set.base + i), scene);
            kprintf("[pa] slot %u %s %u regs(32)\n", i, play ? "on" : "init", scene.count);
        } else {
            const dby_pa_bank &bank = kDbyPaBank[set.bank][i];
            const dby_pa_scene &scene = play ? bank.on : bank.init;
            runScene(set, static_cast<uint8_t>(set.base + i), scene);
            kprintf("[pa] slot %u %s %u regs\n", i, play ? "on" : "init", scene.count);
        }
        ++ready;
    }
    return ready;
}

void runInit(const PaSet &set, uint32_t found) {
    kprintf("[pa] %s init done slots=%u\n", set.name, runBankScene(set, found, false));
}
/* ================= ADSP 只读调查 =================
 * power.md §2 里 sheng-adsp=1 那一档做的事：只看启动阶段留下了什么，一个字节也不写。
 * 地址、条目号、结构体偏移全部出自本机（kona/SM8250）厂商内核 4.19 的原文：
 *   kona.dtsi:983   smem_region@80900000 + 0x200000（no-map）
 *   kona.dtsi:972   cmd-db@80860000 + 0x20000（no-map）
 *   kona.dtsi:1053  pil_adsp_region@8a100000 + 0x1d00000（no-map）
 *   kona.dtsi:4386  qcom,lpass@17300000 = qcom,pil-tz-generic：pas-id=1、smem-id=423、sysmon-id=1、
 *                   ssctl-instance-id=0x14、firmware-name="adsp"、proxy 轨 L11A(vdd_cx)+L4A(vdd_mx) 都投
 *                   RPMH_REGULATOR_LEVEL_TURBO、proxy 时钟 RPMH_CXO_CLK、proxy-timeout-ms=10000、
 *                   mboxes=<&qmp_aop 0> "adsp-pil"、interrupts <&pdc 6> + adsp_smp2p_in 0/2/1/3、
 *                   smem-states=<&adsp_smp2p_out 0>（force-stop）
 *   kona-smp2p.dtsi:9  qcom,smem=<443>,<429>；local-pid=0；remote-pid=2
 *   结构体偏移在下面每个常量旁边按字段顺序算出来，来源是 drivers/soc/qcom/{smem.c,smp2p.c,cmd-db.c}
 *
 * sheng/alioth 的 AdspProbe 是 SM8550 的 PAS（kAdspBase 0x9ea00000、SMEM 0x81d00000、ARC lcx/lmx 代理轨、
 * MIAM 邮箱在 0xc300000），本机是 PIL-TZ，一个常量都不能搬；只有 SMEM 条目号 443/429 和 crash id 423 恰好一致。
 *
 * 故意没读的两块：LPASS CSR 0x17300000（pil-tz 的 reg 基址；ADSP 没上电时读它会不会总线挂死，厂商原文里
 * 查不到依据，不拿整机去试）、IPCC 0x408000（本机没有驱动原文给寄存器语义，读回来也只是裸数据）。
 */
constexpr uint64_t kSmem = 0x80900000, kSmemSize = 0x200000;
constexpr uint64_t kCmdDb = 0x80860000, kCmdDbSize = 0x20000;
constexpr uint64_t kPilAdsp = 0x8a100000, kPilHead = 0x4000;   // 只看头 16 KiB：0x1d00000 全扫没有意义
constexpr uint32_t kItemSmp2pIn = 443, kItemSmp2pOut = 429, kItemRestart = 423;
constexpr uint32_t kRemoteAdsp = 2, kLocalApps = 0, kGlobalHost = 0xfffeu;   // SMEM_GLOBAL_HOST，smem.c:83
// 小端把四个字节读成一个字："$TOC"={24,54,4f,43}（smem.c:172）、"$PRT"={24,50,52,54}（:209）、
// "$SMP"=SMP2P_MAGIC 0x504d5324（smp2p.c:57）、cmd-db magic {db,30,03,0c}（cmd-db.c:98）。
constexpr uint32_t kMagicToc = 0x434f5424u, kMagicPart = 0x54525024u;
constexpr uint32_t kMagicSmp2p = 0x504d5324u, kMagicCmdDb = 0x0c0330dbu;
constexpr uint32_t kCanary = 0xa5a5u;   // SMEM_PRIVATE_CANARY，半个字（smem.c:228）
/* struct smem_header（smem.c:128）：proc_comm[4]，每项 {command,status,params[2]} 16 字节 = 64；
 * version[32] 在 64..191（SBL 下标 7、global heap 11、global part 12，smem.c:66-74）；
 * initialized@192、free_offset@196、available@200、reserved@204；toc@208，
 * 每项 struct smem_global_entry {allocated,offset,size,aux_base} = 16 字节，SMEM_ITEM_COUNT 512。 */
constexpr uint32_t kHdrInit = 192, kHdrToc = 208, kTocStride = 16;
/* struct smem_ptable（smem.c:165）：magic@0、version@4、num_entries@8、reserved[5]@12..31，entry@32；
 * 每项 struct smem_ptable_entry {offset,size,flags,host0@12,host1@14,cacheline@16,reserved[7]} = 48 字节。
 * 分区表在 SMEM 区最后 4 KiB（smem.c:1055 就 ioremap 末尾一页）。 */
constexpr uint32_t kPtableBase = kSmemSize - 0x1000, kPtableEntry = 32, kPtableStride = 48, kPtableMax = 32;
/* struct smem_partition_header（smem.c:181）32 字节：magic@0、host0@4、host1@6、size@8、
 * offset_free_uncached@12、offset_free_cached@16、reserved[3]。
 * struct smem_private_entry（smem.c:220）16 字节：canary@0（半字）、item@2（半字）、size@4、
 * padding_data@8（半字）、padding_hdr@10（半字）、reserved@12；
 * 第一条从 phdr+32 开始（phdr_to_first_uncached_entry），走到 phdr+offset_free_uncached 为止，
 * 下一条 = 本条 + 16 + padding_hdr + size（uncached_entry_next），数据 = 本条 + 16 + padding_hdr。 */
constexpr uint32_t kPartHdr = 32, kPrivHdr = 16, kPrivMax = 1024;

uint32_t rd16(volatile uint32_t *base, uint32_t offset) {
    return (rd(base, offset & ~3u) >> ((offset & 3u) * 8)) & 0xffffu;
}

uint32_t rd8(volatile uint32_t *base, uint32_t offset) {
    return (rd(base, offset & ~3u) >> ((offset & 3u) * 8)) & 0xffu;
}

/* 在分区的 uncached 段里找一个 SMEM 条目，照 smem.c:538 qcom_smem_get_private() 的走法。
 * status：1 找到、2 canary 坏（厂商在这儿直接判分区无效）、3 越界或尺寸不合理、0 走完没有。 */
struct Item {
    uint32_t status, dataAt, dataSize, stopAt, walked;
};

Item findItem(volatile uint32_t *p, uint32_t size, uint32_t freeUncached, uint32_t id) {
    Item out = {0, 0, 0, 0, 0};
    uint32_t at = kPartHdr;
    for (unsigned n = 0; n < kPrivMax; ++n) {
        if (at + kPrivHdr > freeUncached || at + kPrivHdr > size) {out.status = 3; out.stopAt = at; return out;}
        if (rd16(p, at) != kCanary) {out.status = 2; out.stopAt = at; return out;}
        const uint32_t dataSize = rd(p, at + 4);
        const uint32_t padHdr = rd16(p, at + 10);
        ++out.walked;
        if (rd16(p, at + 2) == id) {
            out.status = 1; out.dataSize = dataSize; out.stopAt = at;
            out.dataAt = at + kPrivHdr + padHdr;   // uncached_entry_to_item()
            return out;
        }
        if (dataSize > size) {out.status = 3; out.stopAt = at; return out;}
        at += kPrivHdr + padHdr + dataSize;
    }
    out.status = 3; out.stopAt = at;
    return out;
}

void textAt(char *dst, unsigned cap, volatile uint32_t *base, uint32_t at, uint32_t limit) {
    size_t n = 0;
    for (; n + 1 < cap && n < limit; ++n) {
        const uint32_t ch = rd8(base, at + n);
        if (!ch) break;
        dst[n] = static_cast<char>(ch >= 32 && ch < 127 ? ch : '?');
    }
    dst[n] = 0;
}

/* struct smp2p_smem_item（smp2p.c:66-88，__packed）：magic@0、version@4（一字节）、features@5..7、
 * local_pid@8、remote_pid@10、total_entries@12、valid_entries@14、flags@16，
 * 之后 SMP2P_MAX_ENTRY=16 组 {u8 name[16]; u32 value;}，每组 20 字节，第一组在 +20。
 * smp2p.c:252 用 strcmp(name, dts 的 qcom,entry-name) 挑 “slave-kernel”，这里把每组原样摊开。 */
void dumpSmp2p(const char *tag, uint32_t id, volatile uint32_t *base, uint32_t at, uint32_t size) {
    const uint32_t magic = rd(base, at);
    kprintf("[pa-adsp] %s/%u smp2p magic=0x%08x ok=%d ver=%u pid=%u/%u total=%u valid=%u flags=0x%08x\n", tag,
            id, magic, magic == kMagicSmp2p, rd8(base, at + 4), rd16(base, at + 8), rd16(base, at + 10),
            rd16(base, at + 12), rd16(base, at + 14), rd(base, at + 16));
    if (magic != kMagicSmp2p) return;
    uint32_t valid = rd16(base, at + 14);
    if (valid > 16) valid = 16;
    for (uint32_t i = 0; i < valid; ++i) {
        const uint32_t entry = at + 20 + i * 20;
        if (entry + 20 > size) break;
        char name[17];
        textAt(name, sizeof name, base, entry, 16);
        kprintf("[pa-adsp] %s/%u entry=%u name=%s value=0x%08x\n", tag, id, i, name, rd(base, entry + 16));
    }
}

void surveyPart(const char *tag, volatile uint32_t *base, uint32_t size, uint32_t cacheline, uint32_t id0,
                uint32_t id1) {
    const uint32_t fuse = rd(base, 12);
    kprintf("[pa-adsp] part %s magic=0x%08x ok=%d hosts=%u/%u size=0x%x free_unc=0x%x free_cch=0x%x cline=0x%x\n",
            tag, rd(base, 0), rd(base, 0) == kMagicPart, rd16(base, 4), rd16(base, 6), rd(base, 8), fuse,
            rd(base, 16), cacheline);
    if (rd(base, 0) != kMagicPart || fuse > size) return;
    const uint32_t ids[3] = {id0, id1, kItemRestart};
    for (unsigned i = 0; i < 3; ++i) {
        const Item it = findItem(base, size, fuse, ids[i]);
        kprintf("[pa-adsp] part %s item=%u status=%u walked=%u at=0x%x size=0x%x data=0x%x\n", tag, ids[i],
                it.status, it.walked, it.stopAt, it.dataSize, it.dataAt);
        if (it.status != 1) continue;
        if (i < 2) {dumpSmp2p(tag, ids[i], base, it.dataAt, it.dataSize); continue;}
        /* subsys-pil-tz.c:809 log_failure_reason() 把 423 当字符串读：第一个字符就是原因，长度取 size，
         * 空串是 “unknown, empty string found”。这里上限 72 个字符，多出来的留在表里不刷屏。 */
        char reason[73];
        textAt(reason, sizeof reason, base, it.dataAt, it.dataSize);
        kprintf("[pa-adsp] part %s restart=%u reason=[%s]\n", tag, ids[i], reason);
    }
}

void surveyAdsp() {
    kprintf("[pa-adsp] dt loader=qcom,pil-tz-generic reg=0x17300000 pas-id=1 smem-id=423 sysmon-id=1 "
            "ssctl=0x14 fw=adsp rails=L11A,L4A=TURBO xo=cxo proxy-timeout=10000 mbox=qmp_aop\n");

    Window smem = {}, cmdb = {}, pil = {};
    if (!mapWindow(smem, kSmem, kSmemSize)) {
        kprintf("[pa-adsp] smem map=0 base=0x%x size=0x%x: nothing below this line is measurable\n",
                static_cast<uint32_t>(kSmem), static_cast<uint32_t>(kSmemSize));
        return;
    }
    volatile uint32_t *s = smem.va;
    /* header：SBL 版本非零说明 ABL 真的初始化过 SMEM；global heap / global part 两个版本号决定
     * qcom_smem_get() 走分区还是走 toc。 */
    kprintf("[pa-adsp] smem init=%u free=0x%x avail=0x%x ver7_sbl=0x%08x ver11_heap=%u ver12_part=%u\n",
            rd(s, kHdrInit), rd(s, kHdrInit + 4), rd(s, kHdrInit + 8), rd(s, 64 + 7 * 4), rd(s, 64 + 11 * 4),
            rd(s, 64 + 12 * 4));

    /* 分区表：smem.c:791 要求 magic "$TOC" 且 version==1；enumerate_partitions（:904）只挑
     * host0/host1 里有本机（0）的条目，remote 那个下标记进 partition_desc[remote]；
     * set_global_partition（:825）只认 host0==host1==0xfffe。 */
    const uint32_t ptAt = kPtableBase;
    const uint32_t ptMagic = rd(s, ptAt), ptVersion = rd(s, ptAt + 4), ptCount = rd(s, ptAt + 8);
    kprintf("[pa-adsp] ptable at=0x%x magic=0x%08x ok=%d version=%u entries=%u\n", ptAt, ptMagic,
            ptMagic == kMagicToc, ptVersion, ptCount);
    if (ptMagic != kMagicToc || ptVersion != 1) return;
    uint32_t peerAt = 0, peerSize = 0, peerCline = 0, globalAt = 0, globalSize = 0, globalCline = 0;
    const uint32_t count = ptCount > kPtableMax ? kPtableMax : ptCount;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = ptAt + kPtableEntry + i * kPtableStride;
        const uint32_t offset = rd(s, e), size = rd(s, e + 4);
        const uint32_t host0 = rd16(s, e + 12), host1 = rd16(s, e + 14), cline = rd(s, e + 16);
        kprintf("[pa-adsp] entry=%u hosts=%u/%u offset=0x%x size=0x%x cline=0x%x\n", i, host0, host1, offset,
                size, cline);
        if (!offset || !size) continue;
        if ((host0 == kLocalApps && host1 == kRemoteAdsp) || (host0 == kRemoteAdsp && host1 == kLocalApps)) {
            peerAt = offset; peerSize = size; peerCline = cline;
        }
        if (host0 == kGlobalHost && host1 == kGlobalHost) {globalAt = offset; globalSize = size; globalCline = cline;}
    }
    if (peerAt) surveyPart("apps-adsp", s + peerAt / 4, peerSize, peerCline, kItemSmp2pIn, kItemSmp2pOut);
    else kprintf("[pa-adsp] part apps-adsp absent: no ptable entry pairs host 0 with 2\n");
    if (globalAt) surveyPart("global", s + globalAt / 4, globalSize, globalCline, kItemSmp2pIn, kItemSmp2pOut);
    else kprintf("[pa-adsp] part global absent: qcom_smem_get(HOST_ANY, 423) would fall back to the toc\n");

    /* 第三种查法（smem.c:642 的最后一支、:865 的数据定位）：header->toc[423]，数据在 SMEM 基址 + offset。 */
    const uint32_t toc = kHdrToc + kItemRestart * kTocStride;
    kprintf("[pa-adsp] toc[423] alloc=0x%08x off=0x%x size=0x%x aux=0x%08x\n", rd(s, toc), rd(s, toc + 4),
            rd(s, toc + 8), rd(s, toc + 12));
    if (rd(s, toc) && rd(s, toc + 4) + rd(s, toc + 8) <= kSmemSize) {
        char reason[73];
        textAt(reason, sizeof reason, s, rd(s, toc + 4), rd(s, toc + 8));
        kprintf("[pa-adsp] toc[423] reason=[%s]\n", reason);
    }

    /* cmd-db：AOP/RPMh 的命令库。没有它就没法给 ADSP 的代理轨投票，也没有 ARC 地址。 */
    if (mapWindow(cmdb, kCmdDb, kCmdDbSize)) {
        const uint32_t ver = rd(cmdb.va, 0), magic = rd(cmdb.va, 4);
        /* struct rsc_hdr（cmd-db.c:53）16 字节，全 le16：slv_id@0、header_offset@2、data_offset@4、cnt@6、
         * version@8；第一项在 magic 之后（base+8）。 */
        kprintf("[pa-adsp] cmdb version=0x%08x magic=0x%08x ok=%d rsc0 id=%u hdr_off=0x%x data_off=0x%x cnt=%u ver=%u\n",
                ver, magic, magic == kMagicCmdDb, rd16(cmdb.va, 8), rd16(cmdb.va, 10), rd16(cmdb.va, 12),
                rd16(cmdb.va, 14), rd16(cmdb.va, 16));
    } else kprintf("[pa-adsp] cmdb map=0 base=0x%x: RPMh votes unavailable\n", static_cast<uint32_t>(kCmdDb));

    /* PIL 区头 16 KiB：ABL/PIL 若真拷过 adsp 段，这里不会全零；ELF magic 0x464c457f 是 "\x7fELF"。 */
    if (mapWindow(pil, kPilAdsp, kPilHead)) {
        unsigned nonzero = 0, elf = 0, elfAt = 0;
        for (unsigned w = 0; w < kPilHead / 4; ++w) {
            const uint32_t value = pil.va[w];
            if (!value) continue;
            ++nonzero;
            if (value == 0x464c457fu && !elf) {elf = 1; elfAt = w * 4;}
        }
        kprintf("[pa-adsp] pil base=0x%x head=0x%x nonzero=%u/%u w0..3=0x%08x,0x%08x,0x%08x,0x%08x elf=%u at=0x%x\n",
                static_cast<uint32_t>(kPilAdsp), static_cast<uint32_t>(kPilHead), nonzero, static_cast<uint32_t>(kPilHead / 4),
                rd(pil.va, 0), rd(pil.va, 4), rd(pil.va, 8), rd(pil.va, 12), elf, elfAt);
    } else kprintf("[pa-adsp] pil map=0 base=0x%x size=0x%x\n", static_cast<uint32_t>(kPilAdsp),
                   static_cast<uint32_t>(kPilHead));
}
}  // namespace

/* 入口。由 BacklightProbe.cpp 在点屏那条初始化序列里调用，参数不在就什么也不做。 */
extern "C" void dby_smartpa_probe(void) {
    static bool attempted;
    if (attempted) return;

    int forced = -1;
    if (!PE_parse_boot_argn("dby-pa", &forced, sizeof forced)) return;
    attempted = true;
    kprintf("[pa] start dby-pa=%d\n", forced);
    /* ADSP 调查放在最前面：它只读，不碰时钟也不碰引脚，PA 那条路早退也影响不到它。
     * 放在这里还有一层原因——dby-pa 是本镜像里唯一已经存在的音频侧开关，不想为调查再加字节的启动参数。 */
    surveyAdsp();

    if (!mapWindow(gGcc, kGcc, 0x60000) || !mapWindow(gSe, kSe, 0x4000) ||
        !mapWindow(gTlmm, kTlmmNorth, 0x100000)) {
        kprintf("[pa] map failed gcc=%d se=%d tlmm=%d\n", gGcc.va != nullptr, gSe.va != nullptr,
                gTlmm.va != nullptr);
        return;
    }
    if (!clocksOn()) {kprintf("[pa] qupv3 wrap0 clocks did not come up\n"); return;}
    /* 分路放开不等于时钟存在：先量 SE3 根时钟，再复引脚、配时序。 */
    startSe3Root();
    if (!configurePins()) {kprintf("[pa] tlmm unavailable\n"); return;}
    programSe3Timing();

    const GeniIo io = {ioRead, ioWrite, const_cast<uint32_t *>(gSe.va)};
    uint32_t config = 0;
    if (geni_i2c_prepare(&gBus, &io, kSets[0].base, &config) != 0) {
        kprintf("[pa] geni not usable proto=%u clk=0x%08x scl=0x%08x ifdis=%u\n", config >> 24,
                config & 0xffffffu, rd(gSe.va, GENI_SCL_COUNTERS), rd(gSe.va, kRegIfDisable) & 1u);
        return;
    }
    gBusReady = true;
    kprintf("[pa] geni ready proto=%u clk=0x%08x\n", config >> 24, config & 0xffffffu);

    /* 候选顺序：dby-pa=0/1/2 定第一个，其余值按 dts 顺序；不管从哪个开始，三套都会试到，
     * 每套内部再分“按自己序列”和“序列反相”两次。第一个有人应答的套跑 INIT 后结束。 */
    unsigned order[kSetCount];
    unsigned nOrder = 0;
    if (forced >= 0 && static_cast<unsigned>(forced) < kSetCount) order[nOrder++] = static_cast<unsigned>(forced);
    for (unsigned i = 0; i < kSetCount; ++i)
        if (i != static_cast<unsigned>(forced)) order[nOrder++] = i;

    for (unsigned o = 0; o < nOrder; ++o) {
        const PaSet &set = kSets[order[o]];
        for (int invert = 0; invert < 2; ++invert) {
            resetPa(set, invert);
            uint32_t ids[kPAs], rcs[kPAs];
            const uint32_t found = probeSet(set, ids, rcs);
            /* 一次套一次电平就一行判词：
             *   alive>0                     → 传输通了，这颗在，直接跑 INIT
             *   rc=2 且 st 的 bit9 以上置位 → 从机不应答：线上有时钟波形，但从机没电或还在复位里
             *   rc=3 且 st=0x00000000       → 引擎一条中断都没发过：SE 的时钟根本不存在
             *   rc=0 而 id=0                → 应答了但 version 读出 0，厂商的存活判据同样算死
             *   rc=1/5                      → 是我们命令时序/状态机的问题，不是硬件 */
            kprintf("[pa] set=%s magic=0x%08x reg=%u val=%u release=%u invert=%d alive=0x%x "
                    "id=0x%08x,0x%08x,0x%08x,0x%08x rc=%u,%u,%u,%u err=%u st=0x%08x\n", set.name, set.magic,
                    set.regBytes, set.valBytes, set.seq[2], invert, found, ids[0], ids[1], ids[2], ids[3],
                    rcs[0], rcs[1], rcs[2], rcs[3], gErrClass, gErrStatus);
            if (found) {
                gPaSet = &set;
                gPaFound = found;
                runInit(set, found);
                kprintf("[pa] done set=%s invert=%d transfers=%u errors=%u\n", set.name, invert, gTransfers,
                        gErrors);
                return;
            }
        }
    }
    kprintf("[pa] no amplifier answered, transfers=%u errors=%u\n", gTransfers, gErrors);
}

/* 播放态写序：DbyAudio 在 ASM 阶梯走完、真的开始推样本那一刻调一次。放在流起来之后而不是跟着
 * INIT 一起，是抄厂商的顺序（tfa98xx.c:2551-2579 在所有 device start 完成后逐颗 UNMUTE）；那颗
 * 调用本身靠 tfa_dev_set_state 的状态缓存去重，所以这里同样只跑一遍，之后再调不碰总线。
 * 返回放开的槽位数，0 表示 PA 那条路根本没通（probe 没命中或 GENI 没就绪）。 */
extern "C" uint32_t dby_smartpa_play(void) {
    static uint32_t ready;
    if (ready) return ready;
    if (!gBusReady || gPaSet == nullptr) return 0;
    ready = runBankScene(*gPaSet, gPaFound, true);
    kprintf("[pa] play set=%s slots=%u transfers=%u errors=%u\n", gPaSet->name, ready, gTransfers, gErrors);
    return ready;
}
