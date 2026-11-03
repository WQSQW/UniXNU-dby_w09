// SPDX-License-Identifier: Apache-2.0
// Ilitek TDDI chip model built from the driver this device's own kernel compiles
// (E:\dby_w09\Code_Opensource\kernel\drivers\devkit\tpkit\hostprocessing\ilitek_thp.c, CONFIG_HUAWEI_THP_ILITEK=y in
// arch/arm64/configs/merge_kona_defconfig). Nothing in the port had ever spoken this protocol, so this fixture is the
// check that the transcription is the vendor's and not an approximation: it records what goes on the wire and what the
// reset line does, and answers the way ilitek_thp.c's own tests require (rxbuf[1] == SPI_ACK at :425, the packet id at
// :431, the four ICE accept conditions at :584-587).
#include "IliTddi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define WIRE_MAX 96
struct wire { int is_read; size_t len; uint8_t bytes[8]; };
static struct wire wire[WIRE_MAX];
static unsigned wire_n, level_n, ms_n, calls, bus_fault = 0xffffffffu;
static unsigned levels[64], ms[64];

// The part.
static int present = 1;   // 0 = nothing is on the bus: MISO stays high, which is what every read of this port returned
static int ice;           // 1 while ICE mode is open: entered by 25+62 10 18, left by 1B+62 10 18 (:552-598)
static int invert;        // 1 = the dummy register answers with the two's complement of what the host wrote
static uint32_t pid_reg = 0x98810000u;   // TDDI_PID_ADDR: ILI9881 in the top half, which is the half :585 tests
static uint32_t dummy_reg;               // IC_DETECT_ADDR, written by the host at :575
static uint32_t selected;                // the address the last 0x25 register write named
static uint8_t served[ILI_XFER_LEN];     // the frame reply an AP-mode read returns
static void reset_model(void);           // defined below, where the core it clears is declared

static void note(const uint8_t *tx, const uint8_t *rx, size_t len, int is_read)
{
    if (wire_n >= WIRE_MAX) return;
    wire[wire_n].is_read = is_read;
    wire[wire_n].len = len;
    for (unsigned k = 0; k < 8; ++k)
        wire[wire_n].bytes[k] = k < len ? (is_read && rx ? rx[k] : tx[k]) : 0;
    ++wire_n;
}

// An idle AP-mode packet: id, then ten points of 0xFF (ilitek_touch.c:931 treats that trio as "not touched").
static void serve_idle_packet(void)
{
    served[1] = ILI_SPI_ACK;
    served[ILI_SHIFT_ID] = ILI_PACKET_ID;
    for (unsigned i = 0; i < ILI_MAX_POINTS; ++i) {
        served[ILI_SHIFT_ID + (4u * i) + 1u] = 0xff;
        served[ILI_SHIFT_ID + (4u * i) + 2u] = 0xff;
        served[ILI_SHIFT_ID + (4u * i) + 3u] = 0xff;
    }
}

static uint32_t ice_value(uint32_t addr)
{
    if (addr == ILI_PID_ADDR) return pid_reg;
    if (addr != ILI_DETECT_ADDR) return 0;
    return invert ? (uint32_t)0 - dummy_reg : dummy_reg;
}

// The three ICE command bytes exactly as the vendor puts them on the wire: ICE_MODE_CTRL_CMD little endian (:553-555).
#define ICE_B0 ((uint8_t)ILI_ICE_CTRL_CMD)                    // 0x62
#define ICE_B1 ((uint8_t)(ILI_ICE_CTRL_CMD >> 8))            // 0x10
#define ICE_B2 ((uint8_t)(ILI_ICE_CTRL_CMD >> 16))           // 0x18

static int xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    (void)ctx;
    if (++calls == bus_fault) return -1;
    assert(len >= 1 && tx && "every Ilitek transfer starts with the write or read command byte");
    if (tx[0] == ILI_SPI_WRITE) {
        const uint8_t *p = tx + 1;                       // touch_driver_spi_write:283-284
        size_t n = len - 1;
        assert(rx == NULL && "the vendor clocks writes with no receive buffer (:286)");
        note(tx, NULL, len, 0);
        if (n >= 4 && p[1] == ICE_B0 && p[2] == ICE_B1 && p[3] == ICE_B2) {   // ICE_MODE_CTRL_CMD (:71)
            ice = p[0] == ILI_ICE_HEADER_MCU_OFF || p[0] == ILI_ICE_HEADER_MCU_ON;
            return 0;
        }
        if (p[0] == ILI_REGISTER_HEAD && n >= ILI_REGISTER_LEN) {       // :334-337
            selected = (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 16);
            if (n > ILI_REGISTER_LEN) {                                 // :338-339, data least significant first
                uint32_t v = 0;
                for (size_t k = ILI_REGISTER_LEN; k < n; ++k) v |= (uint32_t)p[k] << (8 * (k - ILI_REGISTER_LEN));
                if (selected == ILI_DETECT_ADDR) dummy_reg = v;
            }
        }
        return 0;
    }
    if (tx[0] != ILI_SPI_READ) { puts("unexpected command byte on the wire"); return -1; }
    assert(rx && "reads are full-duplex (:314)");
    // The reply, byte for byte as the vendor indexes it: [0] is the byte clocked while 0x83 goes out, and the part's
    // own stream starts at [1]. An absent part holds MISO high, which is the idle signature this board has shown.
    for (size_t k = 0; k < len; ++k) rx[k] = present ? 0x00 : 0xff;
    if (ice && len == ILI_DUMMY_LEN + 4) {               // touch_driver_ice_mode_read:516,522-526 (no ACK, no header)
        uint32_t v = present ? ice_value(selected) : 0xffffffffu;
        rx[1] = (uint8_t)v; rx[2] = (uint8_t)(v >> 8); rx[3] = (uint8_t)(v >> 16); rx[4] = (uint8_t)(v >> 24);
    } else if (len == ILI_XFER_LEN) {                    // touch_driver_get_frame:671
        for (size_t k = 0; k < len; ++k) rx[k] = present ? served[k] : 0xff;
    }
    note(tx, rx, len, 1);
    return 0;
}

static void delay_ms(void *ctx, unsigned v) { (void)ctx; if (ms_n < sizeof ms / sizeof *ms) ms[ms_n++] = v; }
static void set_level(void *ctx, unsigned level)
{
    (void)ctx;
    assert(level_n < sizeof levels / sizeof *levels && "the model only records a bounded number of edges");
    levels[level_n++] = level;
}

static const struct ili_bus bus = {NULL, xfer, delay_ms};
static const struct ili_reset reset = {NULL, set_level, delay_ms};
static const struct ili_timing timing = {10, 5};      // DT ilitek branch: boot_reset_hi/low_delay_ms = 0x0a / 0x05
static uint8_t scratch[ILI_SCRATCH_LEN];
static uint8_t packet[ILI_XFER_LEN];
static struct ili_point pts[ILI_MAX_POINTS];
static struct ili_core core = {.bus = &bus, .reset = &reset, .timing = &timing, .scratch = scratch,
                               .detect_by_dummy_reg = 1};   // DT ilitek branch: detect_by_dummy_reg = <1>

static void reset_model(void)
{
    wire_n = level_n = ms_n = calls = 0; bus_fault = 0xffffffffu;
    present = 1; ice = 0; invert = 0; pid_reg = 0x98810000u; dummy_reg = 0; selected = 0;
    memset(served, 0, sizeof served); memset(levels, 0, sizeof levels); memset(ms, 0, sizeof ms);
    // The core is static, and its evidence counters are what the assertions read, so they are part of the fixture
    // state too: every test starts from a core that has never spoken to a part.
    core.chip_id = core.dummy_readback = 0;
    core.ice_rounds = core.resets = core.reads = core.ack_fail = core.id_fail = 0;
    core.packet_id = 0; memset(core.reply, 0, sizeof core.reply);
}

static void dump_wire(const char *why)
{
    printf("  wire (%s):\n", why);
    for (unsigned i = 0; i < wire_n; ++i)
        printf("    %2u %s len=%2zu %02x %02x %02x %02x %02x %02x %02x %02x\n", i,
               wire[i].is_read ? "R" : "W", wire[i].len, wire[i].bytes[0], wire[i].bytes[1], wire[i].bytes[2],
               wire[i].bytes[3], wire[i].bytes[4], wire[i].bytes[5], wire[i].bytes[6], wire[i].bytes[7]);
    fflush(stdout);
}

// The nth write whose payload starts with `head`, or NULL.
static const uint8_t *nth_write(unsigned n, uint8_t head)
{
    unsigned seen = 0;
    for (unsigned i = 0; i < wire_n; ++i) {
        if (wire[i].is_read || wire[i].bytes[0] != ILI_SPI_WRITE) continue;
        if (wire[i].bytes[1] != head) continue;
        if (seen++ == n) return wire[i].bytes;
    }
    return NULL;
}

static unsigned count_reads_of_len(size_t len)
{
    unsigned n = 0;
    for (unsigned i = 0; i < wire_n; ++i) if (wire[i].is_read && wire[i].len == len) ++n;
    return n;
}

int main(void)
{
    // 1. Detection of a present part. The vendor's order is: reset, enter ICE with the MCU stopped (25 62 10 18),
    //    read 0x4009C, then - because this board sets detect_by_dummy_reg - write and re-read 0x51024, and only then
    //    test the four accept conditions. Leaving ICE is 1B 62 10 18 followed by one more reset (:650-653).
    reset_model();
    assert(ili_detect(&core) == ILI_OK);
    dump_wire("detect, part present");
    assert(core.ice_rounds == 1 && "the pid has ILI9881 in its top half, so round 1 must already accept");
    assert(core.chip_id == pid_reg);
    assert(core.resets == 2 && "one reset in the round plus the exit reset touch_driver_chip_detect() issues");
    // The reset waveform is the vendor's: high, low, high (touch_driver_hw_reset:141-146) with this board's 10/5/10 ms.
    assert(level_n == 6 && ms_n == 6);
    for (unsigned k = 0; k < 2; ++k) {
        assert(levels[3 * k + 0] == 1 && levels[3 * k + 1] == 0 && levels[3 * k + 2] == 1);
        assert(ms[3 * k + 0] == 10 && ms[3 * k + 1] == 5 && ms[3 * k + 2] == 10);
    }
    const uint8_t *enter = nth_write(0, ILI_ICE_HEADER_MCU_OFF);
    assert(enter && enter[2] == ICE_B0 && enter[3] == ICE_B1 && enter[4] == ICE_B2 &&
           "enter ICE is 82 25 62 10 18, ICE_MODE_CTRL_CMD little endian");
    const uint8_t *sel = nth_write(1, ILI_REGISTER_HEAD);
    assert(sel && sel[2] == 0x9C && sel[3] == 0x00 && sel[4] == 0x04 && "then the pid register is addressed (0x4009C)");
    const uint8_t *dwrite = nth_write(2, ILI_REGISTER_HEAD);
    assert(dwrite && dwrite[2] == 0x24 && dwrite[3] == 0x10 && dwrite[4] == 0x05 &&
           "detect_by_dummy_reg writes 0x51024");
    assert(dwrite[5] == 0xA5 && dwrite[6] == 0x5A && dwrite[7] == 0x5A && "with IC_DETECT_DATA least significant first");
    const uint8_t *leave = nth_write(0, ILI_ICE_HEADER_DISABLE);
    assert(leave && leave[2] == ICE_B0 && leave[3] == ICE_B1 && leave[4] == ICE_B2 && "leaving ICE is 1B 62 10 18");
    printf("detect: %u rounds, %u resets, 10/5/10 ms, ICE 25->1B, pid 0x%08x\n",
           core.ice_rounds, core.resets, core.chip_id);

    // 2. Nothing on the bus. MISO floats high, so the pid reads 0xFFFFFFFF and the dummy register reads 0xFFFFFFFF
    //    instead of either pattern - neither is an accept condition, so all three rounds run and the part is refused.
    //    This is the case every previous build of this port was actually in.
    reset_model();
    present = 0;
    assert(ili_detect(&core) == ILI_ERR_CHIP);
    assert(core.ice_rounds == ILI_ICE_RETRY && "three rounds, ENABLE_ICE_MODE_RETRY");
    assert(core.resets == ILI_ICE_RETRY && "no exit reset: the chip detect fails before leaving ICE");
    printf("absent part: %u rounds then ILI_ERR_CHIP (no false accept on all-0xFF reads)\n", core.ice_rounds);

    // 3. The complement rule (:584, rdata == (u32)-wdata) is an accept on its own, and it must fire even with a pid
    //    that says nothing - that is how a pid-masked part still gets detected.
    reset_model();
    pid_reg = 0;
    invert = 1;
    assert(ili_detect(&core) == ILI_OK && core.ice_rounds == 1);
    assert(core.chip_id == 0 && "the pid contributed nothing to this accept");
    assert(core.dummy_readback == (uint32_t)0 - ILI_DETECT_DATA);
    puts("accept conditions: complement of IC_DETECT_DATA accepts with a silent pid");

    // 4. Every transfer of a successful detect is a chance for the bus to refuse, and each refusal must be reported
    //    as a bus error rather than as a missing chip - the mistake the Novatek layer's own test guards against.
    //    A successful detect puts seven transfers on the wire (enter ICE, pid select, pid read, dummy write,
    //    dummy select, dummy read, leave ICE), so fail them one at a time.
    reset_model();
    assert(ili_detect(&core) == ILI_OK);
    const unsigned total = calls;
    for (unsigned f = 1; f <= total; ++f) {
        reset_model();
        bus_fault = f;
        assert(ili_detect(&core) == ILI_ERR_BUS);
    }
    printf("bus fault: each of the %u detect transfers refuses -> ILI_ERR_BUS, never ILI_ERR_CHIP\n", total);

    // 5. A frame read: ACK at reply[1], packet id at reply[3], and the packet moved to the front for the report
    //    decoder. One transfer only - touch_driver_get_frame() is a bare spi_read of frame_size bytes (:671).
    reset_model();
    assert(ili_detect(&core) == ILI_OK);
    serve_idle_packet();
    uint8_t id = 0;
    assert(ili_read_frame(&core, packet, &id) == ILI_OK && id == ILI_PACKET_ID);
    assert(count_reads_of_len(ILI_XFER_LEN) == 1 && "one 0x83 burst of command byte + 43 packet bytes");
    assert(packet[0] == ILI_PACKET_ID && "the 43-byte report ilitek.h counts starts with its id");
    assert(ili_report(packet, 1600, 2560, pts, ILI_MAX_POINTS) == 0 &&
           "an idle packet must not invent a contact");
    puts("frame: 0x83 read, ACK 0xA3, packet id 0x5A, idle packet yields zero points");

    // 6. A broken ACK and a wrong packet id are reported apart, and both say what the part actually sent.
    served[1] = 0x00;
    assert(ili_read_frame(&core, packet, &id) == ILI_ERR_ACK && core.ack_fail == 1);
    served[1] = ILI_SPI_ACK;
    served[ILI_SHIFT_ID] = 0xAA;                 // GESTURE_PACKET_ID: a wake gesture, not a touch report
    assert(ili_read_frame(&core, packet, &id) == ILI_ERR_ID && id == 0xAA && core.id_fail == 1);
    printf("misframed reply: ack_fail=%u id_fail=%u, last reply %02x %02x %02x %02x\n",
           core.ack_fail, core.id_fail, core.reply[0], core.reply[1], core.reply[2], core.reply[3]);

    // 7. The report itself. ilitek_touch.c:937-946: x = ((b0 & 0xF0) << 4 | b1) * panel_wid / TPD_WIDTH, y likewise
    //    from the low nibble, id = slot index, pressure = the weight byte plus 0x1CF.
    memset(packet, 0, ILI_PACKET_LEN);
    packet[0] = ILI_PACKET_ID;
    for (unsigned i = 0; i < ILI_MAX_POINTS; ++i) {       // every slot untouched first
        packet[(4 * i) + 1] = 0xff; packet[(4 * i) + 2] = 0xff; packet[(4 * i) + 3] = 0xff;
    }
    packet[(4 * 2) + 1] = 0x77;                  // x high nibble 7, y high nibble 7
    packet[(4 * 2) + 2] = 0x80;                  // xop = 0x780 = 1920 of TPD_WIDTH 2048
    packet[(4 * 2) + 3] = 0x00;                  // yop = 0x700 = 1792 of TPD_HEIGHT 2048
    packet[(4 * 2) + 4] = 0x31;                  // weight
    int n = ili_report(packet, 1600, 2560, pts, ILI_MAX_POINTS);
    assert(n == 1 && pts[0].id == 2 && pts[0].event == 1);
    assert(pts[0].x == (int)(1920u * 1600u / ILI_TPD_WIDTH) && "xop * abs_max_x / TPD_WIDTH");
    assert(pts[0].y == (int)(1792u * 2560u / ILI_TPD_HEIGHT) && "yop * abs_max_y / TPD_HEIGHT");
    assert(pts[0].x == 1500 && pts[0].y == 2240);
    assert(pts[0].pressure == 0x31 + 0x1CF);
    // A raw value at the top of the 12-bit field must not run past the axis maximum the digitizer advertises.
    packet[(4 * 2) + 1] = 0xff;                  // 0xFF 0x00 0x00 is not the untouched trio: a point, at full scale
    n = ili_report(packet, 1600, 2560, pts, ILI_MAX_POINTS);
    assert(n == 1 && pts[0].x == 1599 && pts[0].y == 2559 && "clipped to abs_max - 1");
    printf("report: slot 2 at (%d,%d) pressure %d, clipped to (%d,%d) at the top of the field\n",
           1500, 2240, 0x31 + 0x1CF, pts[0].x, pts[0].y);

    puts("PASS: Ilitek TDDI layer against the vendor driver's own ICE sequence, framing and report layout");
    return 0;
}
