// Pinned copy of UniXNU/ShengNovatekTouch@a1a6f2b1912b3cde2dcbe96439e5833b9ab250f4 (NvtProtocol.h), carried in this package because a format-1 board does not
// materialise upstream components into the build tree. Keep in sync with components.toml's revision.
// SPDX-License-Identifier: Apache-2.0
// NT36532E host-download protocol over an abstract SPI transport. Register facts follow the study of the
// Linux nt36532e driver (no code copied); see NOTICE. No board addresses or clocks here.
#ifndef NVT_PROTOCOL_H
#define NVT_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>

#define NVT_TRANSFER_LEN (63*1024)
// Largest single write message this driver emits (see nvt_write): the engine only completes short full-duplex bursts, so a
// 63 KiB SRAM image goes out as 256-byte messages, each with its own page register and offset.
#define NVT_BUS_WRITE_MAX 256
// One THP frame of THIS panel, straight out of its own factory library. libafehalW2599Z2700.so thp_afe_get_frame
// (@0x159c8) indexes a size table the library fills in InitialProject (@0x31cc) and passes that value to the SPI
// transport: the table holds 3254 for the frame kind it reads (0xcb6 written into the array at .bss+0xc0), and
// rwSPIBuf adds three bytes (@0x15b0c, add w8,w8,#3) for the command/dummy. 3254 = 54 + 3200.
// The 257/5160/5417 layout that used to sit here is the sheng board's NT36532E frame (40*60 int16 plus its own
// 257-byte sub-frame header); reading this part with it lands the matrix decode 267 bytes off and 16 bits wide.
#define NVT_THP_HEADER_LEN 54
#define NVT_THP_PAYLOAD_LEN 3200
#define NVT_THP_FRAME_LEN (NVT_THP_HEADER_LEN+NVT_THP_PAYLOAD_LEN)
#define NVT_THP_MATRIX_OFFSET NVT_THP_HEADER_LEN
// What the frame buffer's first bytes are is not a convention, it is a check the IC itself passes or fails, and
// thp_afe_get_frame shows the check: @0x15f10-0x15f60 loads buf[6], buf[7], buf[8], buf[9] and ANDs four compares
// (cmp #0xa5, #0x5a, #0x5a, #0xa5) into the bit that decides whether the frame is used at all. Nothing else in the
// header is a gate - the counter pair at buf[4]/buf[5] (@0x15f50-0x15f8c: the same base plus 4 and 5, bfi #8, then
// compared against the expectation at [ctx+380]) is only consulted for a replay.
#define NVT_THP_SYNC0 0xa5
#define NVT_THP_SYNC1 0x5a
#define NVT_THP_SYNC2 0x5a
#define NVT_THP_SYNC3 0xa5
// The payload is 3200 bytes and the raw array is 1600 nodes of 16 bits, not 800 of 32: this unit's project is
// W2599Z2700 (thp_project_id_match_rules.xml:1224, feature 0x78 - bit5 MULTI_FREQ_NOISE set) and the per-project
// caplimit CSV (W2599Z1100_caplimit_default.csv, same schema) declares row_col_num 50,32 = 1600 entries with
// raw_max around 3691-3730, values that do not need 32 bits. 1600 = 16 TX (guiTX_SIZE) x 50 RX (guiRX_SIZE) x 2
// frequencies, so the 32 "rows" are the 16 electrodes sampled twice. Which of the two ways a firmware can order
// that pair - interleaved per electrode, or two 800-node blocks - and whether the 16-bit nodes are big or little
// endian is a property of the image, not of the protocol, so it is scored and latched (nvt_thp_roughness), never
// chosen here.
#define NVT_THP_RAW_NODES 1600
enum {NVT_THP_V_LE_PAIR, NVT_THP_V_LE_BLOCK, NVT_THP_V_BE_PAIR, NVT_THP_V_BE_BLOCK, NVT_THP_V_LE32, NVT_THP_V_BE32, NVT_THP_V_KINDS};
#define NVT_MAX_PARTITIONS 64
// Every read is issued as a 256-byte message. The probe's own record on this board is a 1026-byte FIFO read failing nine
// times in a row while 256 bytes completed, and a 63 KiB burst is exactly what the download needs - so the frame is read
// in 13 short bursts from event_buf (each one re-addresses the chip, which is legal: the event buffer is addressable at
// any offset) instead of one long one that the engine never finished.
#define NVT_READ_MAX 256
// The 0x20 frame command is one atomic full-duplex burst for the entire frame (NVT_THP_FRAME_LEN + 1): it cannot be split,
// because the part streams the image only for the duration of that single command. It therefore needs its own ceiling -
// capping it at NVT_READ_MAX (the register-read chunk size) rejected every frame before it reached the bus.
#define NVT_FRAME_READ_MAX 8704

#define NVT_ERR_BUS (-5)
#define NVT_ERR_PARAM (-22)
#define NVT_ERR_FW (-8)
#define NVT_ERR_CHIP (-19)
#define NVT_ERR_TIMEOUT (-110)

// Transport: full-duplex SPI mode 0, one transfer, chip select held for the whole transfer. rx may alias nothing.
struct nvt_bus {
    void *ctx;
    int (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
    void (*delay_ms)(void *ctx, unsigned ms);
};

struct nvt_reg {uint32_t addr; uint8_t mask;};
struct nvt_map {
    const char *name;
    uint32_t event_buf, boot_rdy, tx_auto_copy_en, spi_dma_tx_info;
    uint32_t ilm_des, ilm_length, dlm_des, dlm_length;
    uint32_t g_ilm_checksum, g_dlm_checksum, r_ilm_checksum, r_dlm_checksum, bld_ilm_dlm_crc;
    struct nvt_reg enb_casc;
};

struct nvt_chip {
    const struct nvt_bus *bus;
    uint8_t *scratch;                 // transmit buffer: caller supplied, at least NVT_TRANSFER_LEN + 8 bytes
    uint8_t *rxbuf;                   // receive buffer: caller supplied, at least NVT_READ_MAX + 2 bytes
    const struct nvt_map *map;
    uint32_t chip_ver_trim, swrst_sif;
    int cascade;                      // cascade variant selected by ENB_CASC_REG
    int hw_crc_len;                   // 3: 3-byte HW CRC bank (NT36532 family)
    uint8_t id[6];
    // Download diagnostics (what the last nvt_download_firmware attempt was doing when it stopped).
    unsigned dl_attempt, dl_stage;    // stage: 1 reset, 2 crc bank, 3 auto-copy enable, 4 firmware write, 5 dma-tx poll,
                                      //        6 bld crc enable, 7 fw crc enable, 8 boot ready, 9 reset-state check
    int dl_rc;                        // result of the failing stage
    uint8_t dl_wait_value;            // last value read while waiting for auto copy to finish
    uint8_t dl_reset_state[6];        // last bytes read from the reset-complete event
    unsigned dl_polls;                // polls spent in the failing wait
    unsigned dma_polls;               // polls spent waiting for SPI_DMA_TX_INFO (stage 5)
    int dma_idle;                     // SPI_DMA_TX_INFO read 0x00 (the stock DMA transport's idle flag)
    // Bootloader CRC state read back after a failed download (nvt_read_bld_hw_crc of the vendor driver).
    uint8_t crc_flags;                // bit2 crc_done, bit0 ilm ok, bit1 dlm ok
    uint32_t crc_bin[2], crc_golden[2], crc_result[2];
    int crc_valid;
    // SRAM read-back after a failed download: 1 = the 32 bytes read match the firmware image. [0] ILM start, [1] ILM end, [2] DLM start.
    int sram_valid;
    uint8_t sram_ok[3];
    int32_t sram_first_bad[2];        // full read-back of ILM/DLM: first differing byte offset (-1 none)
    uint32_t sram_bad[2], sram_crc[2];// differing byte count and the CRC of the read-back words (same algorithm as the image CRC)
    unsigned reset_retries;           // reset-state polls before giving up (0 = the vendor's 10); one poll every 10 ms
};

struct nvt_fw_partition {
    char name[12];
    uint32_t bin_addr, sram_addr, size, crc;
};
struct nvt_fw {
    const uint8_t *data;
    size_t size;
    unsigned count;                   // partitions
    int cascade_2nd_header;
    struct nvt_fw_partition part[NVT_MAX_PARTITIONS];
};
// Read the bootloader's CRC flags and golden/result checksums (the vendor driver prints these when a download fails).
int nvt_read_bld_crc(struct nvt_chip *c, const struct nvt_fw *fw);
// Compare small windows of the downloaded ILM/DLM in SRAM with the image (integrity check of the bulk transfers).
int nvt_verify_sram(struct nvt_chip *c, const struct nvt_fw *fw);

// Host-side CRC used for ILM/DLM (word-wise CRC-32, polynomial 0x04c11db7, covers (size+4)/4 words).
uint32_t nvt_crc32_words(const uint8_t *data, size_t len, size_t avail);   // bytes beyond avail read as 0

int nvt_write(struct nvt_chip *c, uint32_t addr, const uint8_t *data, size_t len);   // sets the page first
int nvt_read(struct nvt_chip *c, uint32_t addr, uint8_t *out, size_t len);
int nvt_write_byte(struct nvt_chip *c, uint32_t addr, uint8_t value);
int nvt_set_page(struct nvt_chip *c, uint32_t addr);

int nvt_fw_parse(struct nvt_fw *fw, const uint8_t *data, size_t size);
// Bootloader reset, read the trim id and select the NT36532 / NT36532E memory map.
int nvt_identify(struct nvt_chip *c);
// Software reset into idle (0xAA at the reset register). The vendor issues this with reset held low on power-on and resume.
int nvt_soft_reset_idle(struct nvt_chip *c);
// Restart the application image out of the bootloader (0xA5 in nt36xxx.c, 0xAA in novatek_thp.c); 20 ms delay.
int nvt_fw_restart(struct nvt_chip *c, uint8_t cmd);
// Full host download with HW CRC and boot-ready handshake; returns 0 once the IC reports reset state >= 0xa0.
int nvt_download_firmware(struct nvt_chip *c, const struct nvt_fw *fw);
// Read one THP frame (NVT_THP_FRAME_LEN bytes) from the event buffer.
int nvt_read_frame(struct nvt_chip *c, uint8_t *frame);
// Extract the 40x60 int16 touch matrix and header fields from a frame.
void nvt_frame_matrix(const uint8_t *frame, int32_t *matrix);
unsigned nvt_frame_type(const uint8_t *frame);
unsigned nvt_frame_counter(const uint8_t *frame);

// The frame gate and node unfolding this panel's own library applies (see NVT_THP_SYNC0 and NVT_THP_RAW_NODES).
// p points at the header as it sits in the burst, so the caller has already found where the burst starts.
int nvt_thp_sync(const uint8_t *p);
unsigned nvt_thp_counter(const uint8_t *p);
// Write the 16x50 electrode grid (NVT_NODES values, row-major) that one candidate layout claims the payload holds.
void nvt_thp_nodes(const uint8_t *payload, unsigned variant, int32_t *matrix);
// The vendor's own acceptance rule for a raw array: neighbouring nodes in a row may not differ much
// (raw_row_delta_max / raw_col_delta_max in the caplimit CSV). Summed absolute neighbour differences therefore rank
// the candidate layouts: the one that is actually this firmware's grid is far the smoothest, and a wrong interleave
// or endianness compares two different electrodes and scores large. Returns the sum over the 16 rows x 49 gaps.
unsigned nvt_thp_roughness(const int32_t *matrix);

// After the download (mirrors the Linux boot flow): firmware info, idle-baseline update, doze delay, doze mode.
#define NVT_EVENT_HOST_CMD 0x50
#define NVT_EVENT_FWINFO 0x78
#define NVT_DOZE_DELAY_ADDR 0x113718
// One candidate event-buffer base, measured rather than assumed. The pinned SM8550 map says 0x125800; this part
// publishes it at 0x22D00 / 0x21C00 / 0x11E00 / 0x11A00 depending on the die (vendor nt36xxx_mem_map.h, and
// trim_id_table at :177 maps the trim id this panel answered with to one of those). Everything the host does is
// derived from event_buf, so a wrong base does not fail loudly: reads return whatever the SRAM holds there.
struct nvt_buf_probe {
    uint32_t base;
    uint32_t first;      // first four bytes at base + NVT_THP_HEADER_LEN-1
    int valid;           // that window passed the THP header check
    int cmd_rc;          // nvt_set_custom_cmd at this base: 0 means the firmware here consumed a host command
};
void nvt_select_event_buf(struct nvt_chip *c, uint32_t base);
int nvt_probe_event_buf(struct nvt_chip *c, uint32_t base, struct nvt_buf_probe *p);

// Result of scanning an address window for a running firmware's event structure.
struct nvt_buf_scan {
    uint32_t base;        // event buffer base of the first match (0 when nothing matched)
    uint8_t state;        // reset-state byte found there (0xA0..0xAF when the firmware is alive)
    unsigned hits;        // total matches in the window
    unsigned blocks_ok;   // blocks that read back
    unsigned blocks_bad;  // blocks the bus refused
};
// Two things only a running firmware writes: the reset state at event_buf+0x60 and the info block at event_buf+0x78,
// whose first two bytes sum to 0xff. Match both while sweeping [lo,hi) in 2 KiB reads. Returns 0 when one matched.
int nvt_scan_event_buf(struct nvt_chip *c, uint32_t lo, uint32_t hi, struct nvt_buf_scan *out);

struct nvt_fw_info {
    uint8_t fw_ver, fw_type;
    uint8_t x_num, y_num;
    uint16_t abs_x_max, abs_y_max, pid;
    int defaulted;                    // info block was inconsistent after retries
};
int nvt_get_fw_info(struct nvt_chip *c, struct nvt_fw_info *info);
// Host command {0xbf, cmd, 0, lo, hi} at the event buffer; the firmware clears the first byte when it has taken it.
int nvt_set_custom_cmd(struct nvt_chip *c, uint8_t cmd, uint16_t value);
int nvt_set_doze_delay(struct nvt_chip *c, uint16_t value);
int nvt_start_scanning(struct nvt_chip *c, struct nvt_fw_info *info);
// Header integrity of a THP frame: the CRC/marker words at header+4/+8 and their complements at +12/+16.
int nvt_frame_header_valid(const uint8_t *frame);
// A frame is announced by a 32-byte window of its THP header (frame[NVT_THP_HEADER_LEN..+32)): cheap to read, it carries the
// integrity words, the counter and the type, so the full 5 KB frame is only read when it is new.
#define NVT_THP_PEEK_LEN 32
int nvt_peek_frame_header(struct nvt_chip *c, uint8_t *hdr);   // hdr: NVT_THP_PEEK_LEN bytes
// THP frame read as this board's own driver does it (novatek_thp.c: touch_driver_get_frame): one command byte
// spi_read_mask(0x20) in the first tx slot, and the reply of that same transfer is the frame. No page select and no
// event-buffer address are involved - the event_buf map belongs to the other Novatek driver, which this part does not run.
#define NVT_N9Z_FRAME_LEN 1024   // the report prefix: header plus every slot; the vendor reads 8208 and parses the same prefix
// The frame loop halves the read until a transfer completes; 64 is the floor - two address/dummy bytes plus twelve
// six-byte slots, i.e. a whole report even for a full hand on the panel.
#define NVT_N9Z_MIN_LEN 64
int nvt_n9z_read_frame(struct nvt_chip *c, uint8_t *out, size_t len);
// One contact of the N9Z report. Six bytes per slot, slot 0 at byte 0 of the burst (the vendor's public NT36523
// driver, map220v/nt36523_win ntinternal.c:167-178, scans ppos = 6*i for ten slots): id in bits 7:3 of the first
// byte, event in bits 2:0 (1 down, 2 move), then x low, y low, the shared high nibble, area and pressure, decoded as
// x = (p[ppos+1] << 4) | (p[ppos+3] >> 4), y = (p[ppos+2] << 4) | (p[ppos+3] & 0xF), in the panel's 1600 x 2560 space.
struct nvt_point {int x, y, id, event, area, pressure;};
// Decode a frame into slots; returns the number of contacts, 0 for an idle frame. `start` is where the first slot was
// found (the driver's buffers differ by one address byte, so this is reported instead of assumed).
int nvt_n9z_report(const uint8_t *frame, size_t len, struct nvt_point *out, int max, int *start);
// The same report decoded the way this part's own Linux driver decodes it (vendor nt36xxx_ops.c
// novatek_irq_bottom_half, :2790-2860). Two rules differ from the format probe above, and both matter:
//  * an unusable slot is SKIPPED, not the end of the list (`continue`, not break): the firmware reuses slot numbers,
//    so a gap in the middle is normal - slot 0 empty with a finger in slot 3 reports a contact for the vendor and
//    nothing for a decoder that stops at the first gap;
//  * the list is bounded by the panel's own spans (`if (input_x > abs_x_max || input_y > abs_y_max) continue`) rather
//    than by a hard-coded size, so the caller supplies the spans it read from the firmware info block or devicetree.
// Slots start at byte 1: byte 0 of the burst is the dummy the vendor discards (`position = 1 + 6 * i`).
// Every accepted contact gets pressure 1, whatever the byte holds - the vendor writes
// `info->fingers[i].pressure = 1; /* 1 is fake pressure for aft */` because its firmware does not fill that field, and
// a zero pressure is what makes a digitiser report get dropped downstream.
// `wdt` reports the vendor's watchdog pattern (nt36xxx_ops.c nvt_wdt_fw_recovery, :2707-2716): bytes 1..6 all 0xFD or
// 0xFE, which is the firmware telling the host it reset itself.
// The leading byte count this die clocks out in front of the report is not known to us, so the same three starts the
// decoder above tries are scanned here too and the one that yields the most contacts is used; `start` reports it.
int nvt_n9z_points(const uint8_t *frame, size_t len, int span_x, int span_y, struct nvt_point *out, int max,
                   int *start, int *wdt);
int nvt_peek_valid(const uint8_t *hdr);
unsigned nvt_peek_counter(const uint8_t *hdr);
unsigned nvt_peek_type(const uint8_t *hdr);
#endif
