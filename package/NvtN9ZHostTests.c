// SPDX-License-Identifier: Apache-2.0
// N9Z chip model (the IC this board actually carries: DTBO fragment@89 support_vendor_ic_type=<5>) built from
// Huawei's own novatek_thp.c. The upstream ShengNovatekTouch fixture models the NB1 series (0x1FB104/0x1FB43E),
// so it cannot accept or reject this board's protocol layer; this file is that missing check.
#include "NvtProtocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// novatek_thp.c ic_info_table[] entry .ic_type = N9Z_SERIES
#define N9Z_SWRST 0x3F0FEu
#define N9Z_TRIM  0x3F004u
// novatek_thp.c trim_id_table[]: {0x20,0xFF,0xFF,0x23,0x65,0x03} mask {1,0,0,1,1,1}
static const uint8_t kTrimId[6] = {0x20, 0xff, 0xff, 0x23, 0x65, 0x03};
// novatek_thp.c touch_driver_bootloader_reset()/touch_driver_sw_reset_idle()
#define kBootloaderReset 0x69
#define kSwResetIdle 0xAA

#define LOG_MAX 64
struct op { int is_write; uint32_t addr; uint8_t first; size_t len; };
static struct op log[LOG_MAX];
static unsigned log_n, page_sel, delays, bus_calls, bus_fault = 0xffffffffu;
static int part_present = 1;   // 0 = the line answers nothing at all (all 0xFF, the idle signature)

static uint32_t resolve(uint8_t offset_byte) { return ((uint32_t)page_sel << 7) | (offset_byte & 0x7fu); }

static void record(int is_write, uint32_t addr, uint8_t first, size_t len) {
    if (log_n < LOG_MAX) log[log_n++] = (struct op){is_write, addr, first, len};
}

static int xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    (void)ctx;
    if (++bus_calls == bus_fault) return -1;
    assert(len >= 2 && "every Novatek SPI phase is at least an offset byte plus one byte");
    if (tx[0] == 0xff && len == 3) {           // page select: spi_write_mask(0x7F), hi, low
        page_sel = ((uint32_t)tx[1] << 8) | tx[2];
        record(1, 0, 0xff, 3);
        return 0;
    }
    uint32_t addr = resolve(tx[0]);
    if (tx[0] & 0x80u) {                        // write phase: spi_write_mask(addr & 0x7F)
        assert(rx == NULL);
        record(1, addr, tx[1], len - 1);
        return 0;
    }
    // Read phase, as the official fixture and this part both do it: the address byte, one dummy byte on MOSI,
    // and the chip's data starting in the reply's third byte.
    assert(rx && len >= 3 && tx[1] == 0);
    rx[0] = rx[1] = 0;
    for (size_t k = 0; k + 2 < len; ++k) {
        uint32_t a = addr + (uint32_t)k;
        uint8_t v = 0x00;
        if (part_present && a >= N9Z_TRIM && a < N9Z_TRIM + 6) v = kTrimId[a - N9Z_TRIM];
        else if (!part_present) v = 0xff;                     // idle/absent part holds MISO high
        rx[2 + k] = v;
    }
    record(0, addr, tx[0], len - 2);
    return 0;
}

static void delay_ms(void *ctx, unsigned ms) { (void)ctx; (void)ms; ++delays; }

static const struct nvt_bus bus = {NULL, xfer, delay_ms};
static uint8_t scratch[NVT_TRANSFER_LEN + 8], rxbuf[NVT_READ_MAX + 8];
static struct nvt_chip chip = {.bus = &bus, .scratch = scratch, .rxbuf = rxbuf};

static void reset_model(void) {
    memset(log, 0, sizeof log); log_n = 0; page_sel = 0; delays = 0; bus_calls = 0; bus_fault = 0xffffffffu;
    part_present = 1;
    memset(scratch, 0, sizeof scratch); memset(rxbuf, 0, sizeof rxbuf);
}

static void dump_log(const char *why) {
    printf("  wire log (%s):\n", why);
    for (unsigned i = 0; i < log_n; ++i)
        printf("    %2u %s addr=%06x first=%02x len=%zu\n", i, log[i].is_write ? "W" : "R",
               log[i].addr, log[i].first, log[i].len);
    fflush(stdout);
}

// The first write to a given resolved address, or -1.
static int first_write(unsigned idx, uint32_t addr) {
    unsigned seen = 0;
    for (unsigned i = 0; i < log_n; ++i) {
        if (!log[i].is_write || log[i].addr != addr) continue;
        if (seen++ == idx) return (int)log[i].first;
    }
    return -1;
}

int main(void) {
    // 1. Identification, as touch_driver_check_chip_ver_trim() does it: 0x69 then 0xAA on the software-reset
    //    register, then the masked trim read, five attempts at most with the vendor's 10 ms waits.
    reset_model();
    int rc0 = nvt_identify(&chip);
    dump_log(rc0 ? "identify FAILED, here is what the fork actually put on the wire" : "identify ok");
    assert(!rc0 && "the fork must identify the N9Z part the vendor driver describes");
    assert(chip.id[0] == 0x20 && chip.id[3] == 0x23 && chip.id[4] == 0x65 && chip.id[5] == 0x03);
    assert(first_write(0, N9Z_SWRST) == kBootloaderReset && "first write must be the bootloader reset 0x69");
    assert(first_write(1, N9Z_SWRST) == kSwResetIdle && "second write must be the software reset to idle 0xAA");
    unsigned trims = 0;
    for (unsigned i = 0; i < log_n; ++i)
        if (!log[i].is_write && log[i].addr == N9Z_TRIM) ++trims;
    assert(trims >= 1 && "the trim register must actually be read");
    puts("identify: 0x69 -> 0xAA -> trim read, matches novatek_thp.c");

    // 2. An absent or idle part answers 0xFF: the vendor's loop gives up after five attempts, and so must we.
    reset_model();
    part_present = 0;
    assert(nvt_identify(&chip) == NVT_ERR_CHIP);
    unsigned resets = 0;
    for (unsigned i = 0; i < log_n; ++i)
        if (log[i].is_write && log[i].addr == N9Z_SWRST && log[i].first == kBootloaderReset) ++resets;
    assert(resets == 5 && "five attempts, like touch_driver_check_chip_ver_trim()");
    printf("absent part: %u bootloader resets then ENODEV-equivalent\n", resets);

    // 3. A bus that fails must be reported as a bus error, not as a missing chip.
    reset_model();
    bus_fault = 2;
    assert(nvt_identify(&chip) == NVT_ERR_BUS);
    puts("bus fault: NVT_ERR_BUS (not mistaken for an absent part)");

    // 4. The frame read must be the vendor's touch_driver_get_frame(): one full-duplex phase whose first
    //    byte is the 0x20 read command with the write bit clear - not an NB1 event-buffer address.
    reset_model();
    static uint8_t frame[NVT_N9Z_FRAME_LEN];
    assert(!nvt_n9z_read_frame(&chip, frame, sizeof frame));
    int saw_cmd = 0;
    for (unsigned i = 0; i < log_n; ++i)
        if (!log[i].is_write && log[i].first == 0x20 && log[i].len == sizeof frame) saw_cmd = 1;
    assert(saw_cmd && "one full-duplex burst, addressed by the 0x20 get-frame command");
    puts("frame read: single full-duplex 0x20 burst, like the vendor");

    // 5. A zero-filled frame carries no contacts; the report decoder must say so instead of inventing slots.
    struct nvt_point pts[10];
    int start = 0;
    memset(frame, 0, sizeof frame);
    assert(nvt_n9z_report(frame, sizeof frame, pts, 10, &start) == 0 && "an all-zero frame must yield no points");
    puts("empty frame: zero contacts");

    dump_log("identify, for the record");
    puts("PASS: N9Z protocol layer against the vendor driver's own register map and reset sequence");
    return 0;
}
