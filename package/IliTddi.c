// SPDX-License-Identifier: BSD-2-Clause-Patent
// Ilitek TDDI host-processing profile - see IliTddi.h for where every constant comes from. This file is a
// transcription of ilitek_thp.c's SPI ops and chip detect plus ilitek_touch.c's AP-mode report; it holds no state of
// its own and performs no hardware access, so the same code runs against a fake part on the build host.
#include "IliTddi.h"
// No <string.h> here on purpose, for the same reason NvtProtocol.c:5 states it: this file is built with -mkernel and
// no Darwin sysroot, so the angle include resolves to the host glibc header, whose __THROW/__nonnull machinery does
// not parse under that target. The routines used below are declared directly instead; on arm64 size_t is unsigned long.
extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memmove(void *dst, const void *src, unsigned long n);
extern void *memset(void *dst, int c, unsigned long n);

// touch_driver_spi_write() (:264-294): a transfer of len + DUMMY_LEN whose first byte is SPI_WRITE and whose payload
// follows it, clocked with no receive buffer. On the wire that is 82 <payload...>, which is the whole of the Ilitek
// write framing - no address phase, no page byte, nothing else.
static int ili_spi_write(struct ili_core *c, const uint8_t *payload, size_t len)
{
    if (len == 0 || len > ILI_SCRATCH_LEN - ILI_DUMMY_LEN) return -1;
    c->scratch[0] = ILI_SPI_WRITE;
    memcpy(c->scratch + ILI_DUMMY_LEN, payload, len);
    if (c->bus->xfer(c->bus->ctx, c->scratch, NULL, len + ILI_DUMMY_LEN)) return ILI_ERR_BUS;
    return ILI_OK;
}

// touch_driver_ice_mode_write() (:323-346): REGISTER_HEAD, the 24-bit address low byte first, then len bytes taken
// from the word least-significant byte first.
static int ili_ice_write(struct ili_core *c, uint32_t addr, uint32_t data, unsigned len)
{
    uint8_t reg[ILI_REGISTER_LEN + 4];
    if (len == 0 || len > 4) return -1;
    reg[0] = ILI_REGISTER_HEAD;
    reg[1] = (uint8_t)addr;
    reg[2] = (uint8_t)(addr >> 8);
    reg[3] = (uint8_t)(addr >> 16);
    for (unsigned i = 0; i < len; ++i) reg[ILI_REGISTER_LEN + i] = (uint8_t)(data >> (8 * i));
    return ili_spi_write(c, reg, ILI_REGISTER_LEN + len);
}

// touch_driver_ice_mode_read() (:486-536): write the four register bytes, then read len + DUMMY_LEN bytes. Unlike the
// AP-mode reply there is no ACK and no header in an ICE reply: the value starts at rxbuf[1] and runs low byte first
// (:522-526). That difference is what separates the two modes on the wire.
static int ili_ice_read(struct ili_core *c, uint32_t addr, uint32_t *data, unsigned len)
{
    uint8_t reg[ILI_REGISTER_LEN], tx[ILI_DUMMY_LEN + 4], rx[ILI_DUMMY_LEN + 4];
    if (len == 0 || len > 4) return -1;
    reg[0] = ILI_REGISTER_HEAD;
    reg[1] = (uint8_t)addr;
    reg[2] = (uint8_t)(addr >> 8);
    reg[3] = (uint8_t)(addr >> 16);
    int rc = ili_spi_write(c, reg, ILI_REGISTER_LEN);
    if (rc) return rc;
    // touch_driver_spi_read() (:296-321) clocks the shared zeroed tx buffer with only position 0 written, into a
    // buffer of its own; the two stay separate here for the same reason they are separate in the vendor.
    memset(tx, 0, len + ILI_DUMMY_LEN);
    tx[0] = ILI_SPI_READ;
    memset(rx, 0, len + ILI_DUMMY_LEN);
    if (c->bus->xfer(c->bus->ctx, tx, rx, len + ILI_DUMMY_LEN)) return ILI_ERR_BUS;
    *data = len == 1 ? rx[1]
                     : (uint32_t)rx[1] | ((uint32_t)rx[2] << 8) | ((uint32_t)rx[3] << 16) | ((uint32_t)rx[4] << 24);
    return ILI_OK;
}

void ili_hw_reset(struct ili_core *c)
{
    // :141-146: high, hi ms, low, low ms, high, hi ms - with the delays this board's devicetree gives the ilitek
    // branch (boot_reset_hi_delay_ms = 10, boot_reset_low_delay_ms = 5).
    c->reset->set_level(c->reset->ctx, 1);
    c->reset->delay_ms(c->reset->ctx, c->timing->hi_ms);
    c->reset->set_level(c->reset->ctx, 0);
    c->reset->delay_ms(c->reset->ctx, c->timing->low_ms);
    c->reset->set_level(c->reset->ctx, 1);
    c->reset->delay_ms(c->reset->ctx, c->timing->hi_ms);
    ++c->resets;
}

// The four bytes touch_driver_ice_mode_ctrl() builds at :552-555: the header plus ICE_MODE_CTRL_CMD little endian.
static void ili_ice_cmd(uint8_t *cmd, uint8_t header)
{
    cmd[0] = header;
    cmd[1] = (uint8_t)ILI_ICE_CTRL_CMD;
    cmd[2] = (uint8_t)(ILI_ICE_CTRL_CMD >> 8);
    cmd[3] = (uint8_t)(ILI_ICE_CTRL_CMD >> 16);
}

int ili_detect(struct ili_core *c)
{
    uint8_t cmd[ILI_REGISTER_LEN];
    unsigned accepted = 0;
    int rc;

    // What makes this branch different from every other reset number in the port: thp_update_rst_gpio() (:606-631),
    // driven by this board's support_rst_gpio_update = <1>, frees the parent's rst_gpio (gpio98) and takes the
    // ilitek child's (gpio136) instead, as a plain output held high. The caller owns that pad; everything below
    // assumes its pulses are the ones the chip actually sees.
    //
    // touch_driver_chip_detect() then runs touch_driver_ice_mode_ctrl(ENABLE, DISABLE) (:558-594). mcu_enable is
    // DISABLE, so the header stays ICE_MODE_ENABLE_CMD_HEADER_MCU_OFF. Each of the three rounds resets the chip
    // first, reads the TDDI pid, and - because this board sets detect_by_dummy_reg = <1> - writes and re-reads the
    // dummy register.
    for (unsigned round = 0; round < ILI_ICE_RETRY && !accepted; ++round) {
        uint32_t id = 0, rdata = 0;
        ++c->ice_rounds;
        ili_hw_reset(c);
        ili_ice_cmd(cmd, ILI_ICE_HEADER_MCU_OFF);
        // The vendor logs a failed transfer and carries on with the round (:562-570), because it has one answer for
        // the caller either way. This probe has two, and telling a dead SPI engine from a silent chip is the whole
        // point of the stage, so a bus refusal stops the detect rather than becoming another ILI_ERR_CHIP.
        rc = ili_spi_write(c, cmd, ILI_REGISTER_LEN);
        if (rc) return rc;
        rc = ili_ice_read(c, ILI_PID_ADDR, &id, 4);
        if (rc) return rc;
        c->chip_id = id;
        if (c->detect_by_dummy_reg) {
            rc = ili_ice_write(c, ILI_DETECT_ADDR, ILI_DETECT_DATA, 4);
            if (rc) return rc;
            rc = ili_ice_read(c, ILI_DETECT_ADDR, &rdata, 4);
            if (rc) return rc;
            c->dummy_readback = rdata;
        }
        // :584-587, the four conditions including the complement, which is the part answering the dummy register
        // with an inverted pattern. id >> 16 is the ILI9881/ILI9882 bridge id inside the TDDI chip.
        accepted = (rdata == ILI_DETECT_DATA || rdata == (uint32_t)0 - ILI_DETECT_DATA ||
                    (id >> 16) == ILI9881_CHIP || (id >> 16) == ILI9882_CHIP);
    }
    if (!accepted) return ILI_ERR_CHIP;
    // Leave ICE (:596-598), then touch_driver_chip_detect()'s own last act, "pull reset to exit ice mode" (:650-653).
    ili_ice_cmd(cmd, ILI_ICE_HEADER_DISABLE);
    rc = ili_spi_write(c, cmd, ILI_REGISTER_LEN);
    if (rc) return rc;
    ili_hw_reset(c);
    return ILI_OK;
}

// One frame transfer in the shape touch_driver_spi_read() gives it: the command byte, then zeros, in a single
// full-duplex spi_transfer. The reply lands in `packet`, which the caller therefore sizes at ILI_XFER_LEN; it holds
// the whole transfer until the packet is moved to the front below, so one buffer serves both.
int ili_read_frame(struct ili_core *c, uint8_t *packet, uint8_t *packet_id)
{
    // touch_driver_get_frame() (:661-676) is that transfer of cd->frame_size bytes and nothing more; the AP-mode
    // packet inside its reply starts at CMD_SHIFT_LEN (:431). The 43 bytes counted from there are id + 10 points x 4
    // bytes + 2 spare (ilitek.h:575,618), the id already being one of them - so the transfer is 3 + 43 = 46 long.
    ++c->reads;
    memset(c->scratch, 0, ILI_XFER_LEN);   // the tx buffer is reused, and only position 0 is ever written (:313)
    c->scratch[0] = ILI_SPI_READ;
    memset(c->reply, 0, sizeof c->reply);  // a refused transfer moved nothing, and the evidence must not show the
    if (c->bus->xfer(c->bus->ctx, c->scratch, packet, ILI_XFER_LEN)) return ILI_ERR_BUS;   // previous frame's bytes
    memcpy(c->reply, packet, sizeof c->reply);
    c->packet_id = packet[ILI_SHIFT_ID];
    if (packet_id) *packet_id = c->packet_id;
    // The ACK is tested before the id, so a part answering with something other than 0xA3 is not filed as a part
    // answering with the wrong packet; touch_driver_send_cmd() tests the same byte at the same index (:425).
    if (packet[ILI_DUMMY_LEN] != ILI_SPI_ACK) {++c->ack_fail; return ILI_ERR_ACK;}
    if (c->packet_id != ILI_PACKET_ID) {++c->id_fail; return ILI_ERR_ID;}
    // Move the packet to the front so that packet[0] is the id, which is the buffer ili_report_ap_mode indexes.
    memmove(packet, packet + ILI_SHIFT_ID, ILI_PACKET_LEN);
    return ILI_OK;
}

int ili_report(const uint8_t *packet, int abs_max_x, int abs_max_y, struct ili_point *out, int cap)
{
    int n = 0;

    for (unsigned i = 0; i < ILI_MAX_POINTS && n < cap; ++i) {
        const uint8_t b0 = packet[(4u * i) + 1u];
        const uint8_t b1 = packet[(4u * i) + 2u];
        const uint8_t b2 = packet[(4u * i) + 3u];
        unsigned xop, yop;
        if (b0 == 0xff && b1 == 0xff && b2 == 0xff) continue;   // the point is not touched (:931-935)
        xop = ((b0 & 0xF0u) << 4) | b1;                          // :937
        yop = ((b0 & 0x0Fu) << 8) | b2;                          // :938
        // :945-946 xop * panel_wid / TPD_WIDTH and yop * panel_hei / TPD_HEIGHT, with the panel size taken from this
        // board's devicetree (thp_input_dev abs_max_x = 1600, abs_max_y = 2560) rather than the panel kit's own
        // properties. A 12-bit raw value can exceed TPD_*, so the result is clipped to the axis maximum the
        // digitizer advertises.
        out[n].x = (int)(xop * (unsigned)abs_max_x / ILI_TPD_WIDTH);
        out[n].y = (int)(yop * (unsigned)abs_max_y / ILI_TPD_HEIGHT);
        if (out[n].x > abs_max_x - 1) out[n].x = abs_max_x - 1;
        if (out[n].y > abs_max_y - 1) out[n].y = abs_max_y - 1;
        out[n].id = (int)i;              // :948 touch_info[i].id = i: the slot index is the tracking id
        out[n].event = 1;                // an AP-mode packet says nothing else: a point in it is a point down
        out[n].area = 1;
        out[n].pressure = (int)packet[(4u * i) + 4u] + 0x1CF;   // :950-953, MT_PRESSURE branch
        ++n;
    }
    return n;
}
