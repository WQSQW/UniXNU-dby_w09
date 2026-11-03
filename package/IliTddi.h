// SPDX-License-Identifier: BSD-2-Clause-Patent
// Ilitek TDDI touch controller, host-processing (Huawei THP) profile - the other supplier this board's devicetree
// describes. Everything below is a transcription of the driver this device's own kernel compiles
// (E:\dby_w09\Code_Opensource\kernel\drivers\devkit\tpkit\hostprocessing\ilitek_thp.c, enabled by
// CONFIG_HUAWEI_THP_ILITEK=y in arch/arm64/configs/merge_kona_defconfig) plus the panel packet format from
// panel/ilitek_tddi/ilitek.h. Each constant cites its line. Nothing here touches hardware: the caller supplies the
// SPI transfer and the reset line, exactly like the vendor's spi_device and gpio.
#ifndef DBY_W09_ILI_TDDI_H
#define DBY_W09_ILI_TDDI_H
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------- wire framing (ilitek_thp.c)
#define ILI_SPI_WRITE 0x82u     // ilitek_thp.c:39 SPI_WRITE
#define ILI_SPI_READ  0x83u     // :40 SPI_READ
#define ILI_SPI_ACK   0xA3u     // :41 SPI_ACK
#define ILI_DUMMY_LEN 1u        // :62 DUMMY_LEN - the byte clocked while the command itself goes out
#define ILI_REGISTER_HEAD 0x25u // :65 REGISTER_HEAD, the ICE-mode register access command
#define ILI_REGISTER_LEN 4u     // :66 REGISTER_LEN: head + 24-bit address, little endian (:334-337)

// ---------------------------------------------------------- ICE mode (ilitek_thp.c)
#define ILI_ICE_CTRL_CMD 0x181062u                    // :71, sent little endian as 62 10 18 after the header
#define ILI_ICE_HEADER_MCU_OFF 0x25u                  // :73 enter ICE with the MCU stopped - what chip_detect uses
#define ILI_ICE_HEADER_MCU_ON 0x1Fu                   // :72
#define ILI_ICE_HEADER_DISABLE 0x1Bu                  // :74 leave ICE
#define ILI_ICE_RETRY 3u                              // :61 ENABLE_ICE_MODE_RETRY
#define ILI_PID_ADDR 0x4009Cu                          // :33 TDDI_PID_ADDR, the TDDI controller id
#define ILI9881_CHIP 0x9881u                           // :34 accepted as chip_id >> 16
#define ILI9882_CHIP 0x9882u                           // :35
#define ILI_DETECT_ADDR 0x51024u                       // :83 IC_DETECT_ADDR, the dummy register
#define ILI_DETECT_DATA 0xA55A5AA5u                    // :84 IC_DETECT_DATA, written then read back

// ---------------------------------------------------------- AP-mode packet (panel/ilitek_tddi/ilitek.h)
#define ILI_PACKET_ID 0x5Au     // :618 P5_X_DEMO_PACKET_ID, the id of a normal touch report
#define ILI_PACKET_LEN 43u      // :575 P5_X_DEMO_MODE_PACKET_LEN: id + 10 points x 4 bytes + 2 spare
#define ILI_MAX_POINTS 10u      // :429 MAX_TOUCH_NUM
#define ILI_TPD_WIDTH 2048u     // :433 the touch resolution the vendor divides the raw x by
#define ILI_TPD_HEIGHT 2048u    // :432 ... and the raw y by
// A frame read returns: reply[0] = the byte clocked while 0x83 itself goes out, [1] = ACK, [2] = header echo,
// [3] = packet id, [4..] packet payload. touch_driver_send_cmd() checks exactly [1] == SPI_ACK and [3] == packet id
// (:425, :431) and copies its data from index CMD_HEADER_LEN + CMD_SHIFT_LEN = 4 (:437).
#define ILI_SHIFT_ID 3u         // CMD_SHIFT_LEN: index of the packet id in the reply
#define ILI_SHIFT_DATA 4u       // CMD_HEADER_LEN + CMD_SHIFT_LEN: index of the first payload byte
// One transfer of this length clocks the command byte plus a whole packet: the packet begins at reply[3] and the
// 43 bytes ilitek.h counts there include its id byte, which is why this is 3 + 43 and not 4 + 43.
#define ILI_XFER_LEN (ILI_SHIFT_ID + ILI_PACKET_LEN)   // 46
#define ILI_SCRATCH_LEN 64u     // >= ILI_XFER_LEN: the largest buffer this file needs in one call

// ---------------------------------------------------------- errors
#define ILI_OK 0
#define ILI_ERR_BUS (-5)        // the SPI engine refused the transfer
#define ILI_ERR_ACK (-6)        // the part answered, but not with SPI_ACK
#define ILI_ERR_ID (-7)         // the packet id is not a touch report
#define ILI_ERR_CHIP (-8)       // ICE mode never entered: this is not an Ilitek part (or it is not answering)

// The two operations the board layer must provide. `xfer` is one full-duplex transfer of exactly `len` bytes with
// chip select asserted for all of them - the vendor's touch_driver_spi_read_write() is a single spi_transfer with
// both buffers and no message splitting (:103-132). rx may be NULL for a write, which is what the vendor passes for
// writes (:286).
struct ili_bus {
    void *ctx;
    int (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
    void (*delay_ms)(void *ctx, unsigned ms);
};

// The reset line. set_level() drives the pad and makes it a plain output - the vendor's gpio_direction_output() and
// gpio_set_value() in touch_driver_hw_reset() (:141-146); the caller owns which pad that is.
struct ili_reset {
    void *ctx;
    void (*set_level)(void *ctx, unsigned level);
    void (*delay_ms)(void *ctx, unsigned ms);
};

// boot_reset_hi_delay_ms / boot_reset_low_delay_ms, read from the devicetree by thp_parse_timing_config().
// This board's ilitek branch (stock dtbo.img, fragment@89, huawei_thp@10/ilitek) says hi = 0x0a, low = 0x05.
struct ili_timing {unsigned hi_ms, low_ms;};

struct ili_point {int x, y, id, event, area, pressure;};   // x/y in the panel's own portrait units, see ili_report()

struct ili_core {
    const struct ili_bus *bus;
    const struct ili_reset *reset;
    const struct ili_timing *timing;
    uint8_t *scratch;                    // caller supplied, at least ILI_SCRATCH_LEN bytes: the transmit buffer
    unsigned detect_by_dummy_reg;        // DT ilitek branch: detect_by_dummy_reg = <1>
    // Detection evidence, kept for the diagnostic page rather than logged and thrown away.
    uint32_t chip_id;                    // TDDI_PID_ADDR as it read in the round that accepted
    uint32_t dummy_readback;             // IC_DETECT_ADDR read back after the write
    unsigned ice_rounds;                 // how many of the three the vendor's loop spent
    unsigned resets;                     // hardware resets issued (3 per round, plus the exit pulse)
    // Frame evidence.
    unsigned reads, ack_fail, id_fail;
    uint8_t reply[8];                    // first eight bytes of the last frame transfer, framing included
    uint8_t packet_id;                   // reply[3] of the last transfer
};

// The vendor's touch_driver_hw_reset(): high, hi ms, low, low ms, high, hi ms (:134-159).
void ili_hw_reset(struct ili_core *c);
// touch_driver_chip_detect() (:633-655) with the ICE-mode half of touch_driver_ice_mode_ctrl(ENABLE, DISABLE)
// (:538-604) inside it: three rounds of {reset, enter ICE with the MCU stopped, read the TDDI pid, and - because
// this board sets detect_by_dummy_reg - write and read back the dummy register}, accepting on any of the four
// conditions the vendor accepts on. Then leave ICE and reset once more, which is what the vendor's comment calls
// "pull reset to exit ice mode". Returns ILI_OK when the part identified itself.
int ili_detect(struct ili_core *c);
// touch_driver_get_frame() (:661-676) is a bare SPI_READ of cd->frame_size bytes; the AP-mode packet inside that
// reply is the one ilitek.h describes. Read one packet and hand back its 43 bytes (first byte = packet id).
// *packet_id receives the id actually seen, so an ILI_ERR_ID still says what the part was sending.
int ili_read_frame(struct ili_core *c, uint8_t *packet, uint8_t *packet_id);   // packet: ILI_XFER_LEN bytes in, ILI_PACKET_LEN usable
// ili_report_ap_mode() (ilitek_touch.c:921-975): four bytes per point after the id -
//   b0 = X[11:8] << 4 | Y[11:8], b1 = X[7:0], b2 = Y[7:0], b3 = pressure - and a point whose first three bytes are
// all 0xFF is not touched. Raw coordinates are 12-bit and the vendor scales them by panel/TPD_*; here the panel size
// is passed in, because it is this board's devicetree that carries it (stock dtbo.img fragment@89, thp_input_dev:
// abs_max_x = 0x640 = 1600, abs_max_y = 0xa00 = 2560) and not this protocol.
// Returns the number of points, 0 for an idle packet.
int ili_report(const uint8_t *packet, int abs_max_x, int abs_max_y, struct ili_point *out, int cap);

#endif
