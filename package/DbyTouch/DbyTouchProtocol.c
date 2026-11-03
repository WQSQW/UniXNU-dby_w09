/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * DbyTouchProtocol.c — pure touch core.  No MMIO, no allocation, no libkern.
 *
 * Compiles both as kernel code (record flags from the XNU build) and as plain
 * host C, which is what scripts/build-dby-touch.py additionally checks.
 */

#include "DbyTouchProtocol.h"

/* ARM64 is weakly ordered and `volatile` orders nothing: without a barrier the
 * reader on another CPU can observe the even sequence counter next to payload
 * words that were written *after* it, which is exactly the torn frame the
 * counter exists to prevent (docs/zh/drivers/sheng-novatek-touch.md:130 — the
 * writer bumps the counter before and after the update and the reader retries
 * while the two reads disagree).  __sync_synchronize() is the compiler builtin
 * for a full fence (dmb ish on arm64, a no-op on the host build) and needs no
 * header, so the pure core stays compilable on the host. */
#define DBY_TOUCH_FENCE()  __sync_synchronize()

/* ------------------------------------------------------------------ */
/* Protocol layer — deliberately unimplemented                        */
/* ------------------------------------------------------------------ */

/* TODO(measure): the identify handshake.
 *
 * The only thing established so far is *which* family answers (README §1) and
 * *how* the transport looks (DbyTouchBoard.h §1).  The actual bytes are the
 * vendor driver's, and the stock kernel image we extracted contains them:
 *
 *   touch_driver_chip_detect / touch_driver_communication_check_for_9896 /
 *   _9897 / _9966, touch_driver_get_project_id_for_989x, touch_driver_ice_mode_read
 *
 * Recover them by disassembling those symbols out of
 * _scratch/touch-ic/kernel.bin, then fill in this function and delete the
 * DBY_TOUCH_ERR_UNPROBED return.  Do not copy a Novatek or FocalTech identify
 * sequence: the register maps are unrelated. */

/* The N9Z version/trim table.  Source: the stock kernel image's table (work/tracks/nvt-spec, the
 * 23-entry copy, not the 22-entry one in the vendor source tree), mask per byte, 0 means "do not
 * compare".  The vendor's check_ver_trim_table compares r_buf[i + 2], i.e. our rx[i + 2]. */
static const uint8_t kN9zTrim[23][6] = {
	{ 0x0C, 0xFF, 0xFF, 0x72, 0x66, 0x03 }, { 0x0C, 0xFF, 0xFF, 0x82, 0x66, 0x03 },
	{ 0x0A, 0xFF, 0xFF, 0x72, 0x65, 0x03 }, { 0x0B, 0xFF, 0xFF, 0x25, 0x65, 0x03 },
	{ 0x0A, 0xFF, 0xFF, 0x72, 0x67, 0x03 }, { 0x0A, 0xFF, 0xFF, 0x82, 0x66, 0x03 },
	{ 0x0B, 0xFF, 0xFF, 0x82, 0x66, 0x03 }, { 0x0A, 0xFF, 0xFF, 0x72, 0x66, 0x03 },
	{ 0x55, 0x00, 0xFF, 0x00, 0x00, 0x00 }, { 0x55, 0x72, 0xFF, 0x00, 0x00, 0x00 },
	{ 0xAA, 0x00, 0xFF, 0x00, 0x00, 0x00 }, { 0xAA, 0x72, 0xFF, 0x00, 0x00, 0x00 },
	{ 0xFF, 0xFF, 0xFF, 0x72, 0x67, 0x03 }, { 0xFF, 0xFF, 0xFF, 0x70, 0x66, 0x03 },
	{ 0xFF, 0xFF, 0xFF, 0x70, 0x67, 0x03 }, { 0xFF, 0xFF, 0xFF, 0x72, 0x66, 0x03 },
	{ 0xFF, 0xFF, 0xFF, 0x25, 0x65, 0x03 }, { 0xFF, 0xFF, 0xFF, 0x70, 0x68, 0x03 },
	{ 0xFF, 0xFF, 0xFF, 0x26, 0x65, 0x03 }, { 0x0B, 0xFF, 0xFF, 0x23, 0x65, 0x03 },
	{ 0x0A, 0xFF, 0xFF, 0x23, 0x65, 0x03 }, { 0x20, 0xFF, 0xFF, 0x23, 0x65, 0x03 },
	{ 0xFF, 0xFF, 0xFF, 0x32, 0x65, 0x03 },
};
static const uint8_t kN9zMask[23][6] = {
	{ 1, 0, 0, 1, 1, 1 }, { 1, 0, 0, 1, 1, 1 }, { 1, 0, 0, 1, 1, 1 }, { 1, 0, 0, 1, 1, 1 },
	{ 1, 0, 0, 1, 1, 1 }, { 1, 0, 0, 1, 1, 1 }, { 1, 0, 0, 1, 1, 1 }, { 1, 0, 0, 1, 1, 1 },
	{ 1, 1, 0, 1, 1, 1 }, { 1, 1, 0, 1, 1, 1 }, { 1, 1, 0, 1, 1, 1 }, { 1, 1, 0, 1, 1, 1 },
	{ 0, 0, 0, 1, 1, 1 }, { 0, 0, 0, 1, 1, 1 }, { 0, 0, 0, 1, 1, 1 }, { 0, 0, 0, 1, 1, 1 },
	{ 0, 0, 0, 1, 1, 1 }, { 0, 0, 0, 1, 1, 1 }, { 0, 0, 0, 1, 1, 1 }, { 1, 0, 0, 1, 1, 1 },
	{ 1, 0, 0, 1, 1, 1 }, { 1, 0, 0, 1, 1, 1 }, { 0, 0, 0, 1, 1, 1 },
};

static int
dby_n9z_trim_matches(const uint8_t *rx)
{
	for (unsigned row = 0; row < 23; row++) {
		int ok = 1;

		for (unsigned i = 0; i < 6; i++) {
			if (kN9zMask[row][i] && rx[i + 2] != kN9zTrim[row][i]) {
				ok = 0;
				break;
			}
		}
		if (ok) {
			return (int)row;
		}
	}
	return -1;
}

/* Writes one byte to an N9Z register: latch the page with {FF, hi, mid}, then {lo|80, value}.
 * Source: vendor novatek_thp.c, the same framing the stock driver uses. */
static int
dby_n9z_write(struct dby_touch_bus *bus, uint32_t addr, uint8_t value)
{
	uint8_t latch[3] = { 0xFF, (uint8_t)((addr >> 15) & 0xFF), (uint8_t)((addr >> 7) & 0xFF) };
	uint8_t data[2]  = { (uint8_t)((addr & 0x7F) | 0x80), value };

	if (bus->xfer(bus->ctx, latch, NULL, 3) != DBY_TOUCH_OK) {
		return DBY_TOUCH_ERR_IO;
	}
	return bus->xfer(bus->ctx, data, NULL, 2) == DBY_TOUCH_OK ? DBY_TOUCH_OK : DBY_TOUCH_ERR_IO;
}

/* Reads len bytes: latch the page, then send the low address with the read bit clear.  The driver
 * puts the answer so that it starts at rx[2] (verified against the vendor disassembly). */
static int
dby_n9z_read(struct dby_touch_bus *bus, uint32_t addr, uint8_t *rx, size_t len)
{
	uint8_t latch[3] = { 0xFF, (uint8_t)((addr >> 15) & 0xFF), (uint8_t)((addr >> 7) & 0xFF) };
	uint8_t tx[32], buf[32];

	if (len == 0 || len > sizeof tx) {
		return DBY_TOUCH_ERR_PARAM;
	}
	if (bus->xfer(bus->ctx, latch, NULL, 3) != DBY_TOUCH_OK) {
		return DBY_TOUCH_ERR_IO;
	}
	for (size_t i = 0; i < len; i++) {
		tx[i] = 0;
	}
	tx[0] = (uint8_t)(addr & 0x7F);
	if (bus->xfer(bus->ctx, tx, buf, len) != DBY_TOUCH_OK) {
		return DBY_TOUCH_ERR_IO;
	}
	for (size_t i = 0; i < len; i++) {
		rx[i] = buf[i];
	}
	return DBY_TOUCH_OK;
}

/* Thin exports for the board side: the chip-enable sequence (DbyTouch.cpp, stage 6+) has to poke
 * two registers that no other caller touches - the chip's reset-to-idle word at 0x3F0FE and the
 * 32-byte header peek at the event buffer - and duplicating the page-latch framing there would let
 * the two copies drift. */
int
dby_touch_n9z_write(struct dby_touch_bus *bus, uint32_t addr, uint8_t value)
{
	if (bus == NULL || bus->xfer == NULL) {
		return DBY_TOUCH_ERR_PARAM;
	}
	return dby_n9z_write(bus, addr, value);
}

int
dby_touch_n9z_read(struct dby_touch_bus *bus, uint32_t addr, uint8_t *rx, size_t len)
{
	if (bus == NULL || bus->xfer == NULL || rx == NULL) {
		return DBY_TOUCH_ERR_PARAM;
	}
	return dby_n9z_read(bus, addr, rx, len);
}

/* Write a run of bytes as ONE SPI transfer: latch the page once, then send the low address (with
 * the write bit) followed by the data, all in a single CS window.
 *
 * Why this exists rather than a loop over dby_n9z_write: the event-buffer mailbox at 0x2FE50 is a
 * record the firmware consumes as a unit (the vendor N9Z kernel sends seven bytes in one
 * touch_driver_spi_read_write, novatek_thp.c:867-880, and the official ShengNovatekTouch driver
 * sends five in one bus_write).  Writing it byte by byte leaves the record observable in a partial
 * state and makes "the firmware ignored it" indistinguishable from "it never arrived whole".
 * dbyBusXfer drops CS at the end of every transfer, so a loop is genuinely N separate transactions,
 * not one. */
int
dby_touch_n9z_write_block(struct dby_touch_bus *bus, uint32_t addr, const uint8_t *data, size_t len)
{
	uint8_t latch[3] = { 0xFF, (uint8_t)((addr >> 15) & 0xFF), (uint8_t)((addr >> 7) & 0xFF) };
	uint8_t tx[64];

	if (bus == NULL || bus->xfer == NULL || data == NULL || len == 0 || len + 1 > sizeof tx) {
		return DBY_TOUCH_ERR_PARAM;
	}
	if (bus->xfer(bus->ctx, latch, NULL, sizeof latch) != DBY_TOUCH_OK) {
		return DBY_TOUCH_ERR_IO;
	}
	tx[0] = (uint8_t)((addr & 0x7F) | 0x80);
	for (size_t i = 0; i < len; i++) {
		tx[1 + i] = data[i];
	}
	return bus->xfer(bus->ctx, tx, NULL, len + 1) == DBY_TOUCH_OK ? DBY_TOUCH_OK : DBY_TOUCH_ERR_IO;
}

int
dby_touch_identify_n9z(struct dby_touch_bus *bus, struct dby_touch_identify *out)
{	static const uint32_t kAddrId  = 0x3F004u;
	static const uint32_t kAddrBoot = 0x3F0FEu;

	if (bus == NULL || bus->xfer == NULL || out == NULL) {
		return DBY_TOUCH_ERR_PARAM;
	}
	for (unsigned attempt = 0; attempt < 5; attempt++) {
		/* The pair matters and the order is not interchangeable: 0x69 puts the chip *into* its
		 * bootloader, 0xAA brings it back to idle.  Sending 0x69 alone parks the chip in the
		 * bootloader and no frame ever arrives. */
		if (dby_n9z_write(bus, kAddrBoot, 0x69) != DBY_TOUCH_OK) {
			return DBY_TOUCH_ERR_IO;
		}
		bus->delay_ms(bus->ctx, 10);
		if (dby_n9z_write(bus, kAddrBoot, 0xAA) != DBY_TOUCH_OK) {
			/* 0x69 landed, so the chip is in its bootloader.  One best-effort retry keeps a single
			 * failed write from costing the panel until the next power cycle. */
			(void)dby_n9z_write(bus, kAddrBoot, 0xAA);
			return DBY_TOUCH_ERR_IO;
		}
		bus->delay_ms(bus->ctx, 10);

		if (dby_n9z_read(bus, kAddrId, out->raw, 8) != DBY_TOUCH_OK) {
			return DBY_TOUCH_ERR_IO;
		}
		out->trim_row = dby_n9z_trim_matches(out->raw);
		out->attempts = attempt + 1;
		if (out->trim_row >= 0) {
			return DBY_TOUCH_OK;
		}
		bus->delay_ms(bus->ctx, 10);
	}
	return DBY_TOUCH_ERR_PROTO;
}

int
dby_touch_identify(struct dby_touch_bus *bus, const char **family_out)
{
	if (family_out != NULL) {
		*family_out = DBY_TOUCH_IC_FAMILY;
	}
	if (bus == NULL || bus->xfer == NULL) {
		return DBY_TOUCH_ERR_PARAM;
	}
	/* This board's panel is a Novatek N9Z (support_vendor_ic_type = 5 in the vendor DT), so the
	 * identify is the N9Z one; the earlier "UNPROBED" placeholder was written before the vendor
	 * source settled what the chip is. */
	return DBY_TOUCH_ERR_UNPROBED;
}

/* A full-duplex transfer of zeroes.  No register is assumed, so this is safe to
 * run against an unknown IC and it is the first thing that proves the GENI SPI
 * path works (stage 3 in README §4). */
int
dby_touch_bus_probe(struct dby_touch_bus *bus, uint8_t *rx, size_t len)
{
	uint8_t tx[64];
	size_t  i;

	if (bus == NULL || bus->xfer == NULL || rx == NULL) {
		return DBY_TOUCH_ERR_PARAM;
	}
	if (len == 0 || len > sizeof(tx)) {
		return DBY_TOUCH_ERR_PARAM;
	}
	for (i = 0; i < len; i++) {
		tx[i] = 0;
	}
	return bus->xfer(bus->ctx, tx, rx, len);
}

/* TODO(measure): frame layout.  Stage 4 in README §4 reads the raw bytes and
 * logs them; the packing is derived from those logs, not from a guess here. */
int
dby_touch_parse_frame(const uint8_t *buf, size_t len, struct dby_touch_frame *out)
{
	if (buf == NULL || out == NULL || len == 0) {
		return DBY_TOUCH_ERR_PARAM;
	}
	/* The driver hands over whatever it read; refuse a report larger than the
	 * fixed ceiling instead of letting a bogus length reach the parse below. */
	if (len > (size_t)DBY_TOUCH_FRAME_MAX) {
		return DBY_TOUCH_ERR_PARAM;
	}
	out->count = 0;
	return DBY_TOUCH_ERR_UNPROBED;
}

/* ------------------------------------------------------------------ */
/* Pure helpers                                                       */
/* ------------------------------------------------------------------ */

int32_t
dby_touch_scale_axis(int32_t raw, unsigned raw_max, unsigned panel_max)
{
	int32_t scaled;

	if (raw_max == 0 || panel_max == 0) {
		return 0;
	}
	if (raw <= 0) {
		return 0;
	}
	if ((unsigned)raw >= raw_max) {
		return (int32_t)(panel_max - 1U);
	}
	/* raw_max / panel_max is the magnification (8 on this board).  Keep the
	 * general form so a panel with a different factor still maps exactly.
	 * Unsigned on purpose: see the note in dby_touch_rotate(). */
	scaled = (int32_t)(((uint64_t)(uint32_t)raw * (uint64_t)panel_max) / (uint64_t)raw_max);
	if (scaled < 0) {
		scaled = 0;
	}
	if ((unsigned)scaled > panel_max - 1) {
		scaled = (int32_t)(panel_max - 1U);
	}
	return scaled;
}

int
dby_touch_rotate(int rotation_kind, unsigned panel_w, unsigned panel_h,
    int32_t *x, int32_t *y)
{
	int32_t in_x, in_y;

	if (x == NULL || y == NULL || panel_w == 0 || panel_h == 0) {
		return DBY_TOUCH_ERR_PARAM;
	}
	in_x = *x;
	in_y = *y;

	/* The subtractions are done unsigned on purpose: the kernel is built with
	 * -fsanitize=signed-integer-overflow, and a signed `panel - 1 - value`
	 * expression makes clang emit a reference to the UBSan minimal runtime that
	 * this driver has no reason to depend on.  Unsigned wrap then cast is exact
	 * for every in-range input. */
	switch (rotation_kind) {
	case 0:
		break;
	case 1:                                  /* 90 deg clockwise */
		*x = (int32_t)((uint32_t)panel_h - 1U - (uint32_t)in_y);
		*y = in_x;
		break;
	case 2:
		*x = (int32_t)((uint32_t)panel_w - 1U - (uint32_t)in_x);
		*y = (int32_t)((uint32_t)panel_h - 1U - (uint32_t)in_y);
		break;
	case 3:                                  /* 270 deg clockwise */
		*x = in_y;
		*y = (int32_t)((uint32_t)panel_w - 1U - (uint32_t)in_x);
		break;
	default:
		return DBY_TOUCH_ERR_PARAM;
	}
	return DBY_TOUCH_OK;
}

void
dby_touch_snapshot_publish(struct dby_touch_snapshot *snap,
    const struct dby_touch_frame *frame, int rotation_kind)
{
	unsigned i;
	uint32_t mask = 0;

	if (snap == NULL || frame == NULL) {
		return;
	}

	snap->seq = snap->seq + 1;               /* odd == write in progress */
	DBY_TOUCH_FENCE();                       /* payload must not precede the counter */

	for (i = 0; i < DBY_TOUCH_MAX_CONTACTS; i++) {
		if (i < frame->count && frame->contact[i].down) {
			int32_t x = dby_touch_scale_axis(frame->contact[i].x,
			                DBY_TOUCH_RAW_W, DBY_TOUCH_PANEL_W);
			int32_t y = dby_touch_scale_axis(frame->contact[i].y,
			                DBY_TOUCH_RAW_H, DBY_TOUCH_PANEL_H);

			(void)dby_touch_rotate(rotation_kind, DBY_TOUCH_PANEL_W,
			    DBY_TOUCH_PANEL_H, &x, &y);
			snap->xs[i]       = x;
			snap->ys[i]       = y;
			snap->pressure[i] = frame->contact[i].pressure;
			snap->ids[i]      = frame->contact[i].id;
			mask |= 1U << i;
		} else {
			snap->xs[i]       = 0;
			snap->ys[i]       = 0;
			snap->pressure[i] = 0;
			snap->ids[i]      = 0;
		}
	}

	/* A count above the fixed slot array would make every reader clamp to a
	 * different number than the writer meant; clamp once, here. */
	snap->count     = frame->count > DBY_TOUCH_MAX_CONTACTS
	                    ? DBY_TOUCH_MAX_CONTACTS : frame->count;
	snap->down_mask = mask;
	DBY_TOUCH_FENCE();                       /* payload must precede the even counter */
	snap->seq       = snap->seq + 1;         /* even again: stable */
}

int
dby_touch_snapshot_read(struct dby_touch_snapshot *snap,
    struct dby_touch_frame *out, unsigned retries)
{
	uint32_t before, after;
	unsigned i;

	if (snap == NULL || out == NULL) {
		return DBY_TOUCH_ERR_PARAM;
	}
	if (retries == 0) {
		retries = 1;
	}

	do {
		before = snap->seq;
		if ((before & 1U) != 0) {
			continue;
		}
		DBY_TOUCH_FENCE();               /* the payload reads must follow the counter */
		out->count = snap->count;
		if (out->count > DBY_TOUCH_MAX_CONTACTS) {
			out->count = DBY_TOUCH_MAX_CONTACTS;
		}
		for (i = 0; i < DBY_TOUCH_MAX_CONTACTS; i++) {
			out->contact[i].x        = snap->xs[i];
			out->contact[i].y        = snap->ys[i];
			out->contact[i].pressure = snap->pressure[i];
			out->contact[i].id       = (uint8_t)snap->ids[i];
			out->contact[i].down     = (snap->down_mask >> i) & 1U;
		}
		DBY_TOUCH_FENCE();               /* ... and the counter re-read must follow them */
		after = snap->seq;
		if (before == after && (after & 1U) == 0) {
			return DBY_TOUCH_OK;
		}
	} while (--retries != 0);

	return DBY_TOUCH_ERR_TIMEOUT;
}

int
dby_touch_should_log(uint32_t frame_index)
{
	/* One line every 120 frames (~1.2 s at a 100 Hz report rate) is enough to
	 * see contacts move without drowning a slow serial console. */
	return (frame_index % 120U) == 0U;
}
