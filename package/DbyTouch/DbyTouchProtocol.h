/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * DbyTouchProtocol.h — pure (no I/O, no allocation, no IOKit) touch core.
 *
 * The split is the one unixnu.com documents for the Novatek example
 * (docs/zh/drivers/sheng-novatek-touch.md:20-40):
 *
 *     <core>          matrix -> contacts, no allocation, no I/O
 *     <protocol>      chip identify / reset / frame, through struct dby_touch_bus
 *     <driver>        MMIO, clocks, pins, DMA, SMMU, threads        (DbyTouch.cpp)
 *     <hid>           contacts -> HID report                        (later stage)
 *
 * Everything in this file can therefore be compiled and exercised on the host
 * with a fake bus, which is how the scale/rotation/snapshot logic below is
 * checked without hardware.
 *
 * WHAT IS STILL UNKNOWN (do not guess — see README "还缺哪些数值"):
 *   - the identify handshake bytes and the product-id register
 *   - the frame request/response layout and the report rate
 *   - the touch report packing (contact slot byte, x/y width, pressure width)
 * The stock kernel binary we already extracted carries the vendor implementation
 * (symbols touch_driver_*, see README §1), so these are recoverable offline;
 * they are deliberately NOT written here.
 */

#ifndef DBY_TOUCH_PROTOCOL_H
#define DBY_TOUCH_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "DbyTouchBoard.h"

/* DbyTouchProtocol.c is C.  The driver (DbyTouch.cpp) is C++ and includes this
 * header, so without this guard its calls would reference Itanium-mangled names
 * ("__Z20dby_touch_scale_axisijj") that the archive does not define: the link
 * would fail, or — worse, if only some symbols were used indirectly — succeed
 * against a different declaration.  Everything in this header is implemented in
 * the C translation unit, so the whole body gets C linkage. */
#ifdef __cplusplus
extern "C" {
#endif

#define DBY_TOUCH_MAX_CONTACTS   10
#define DBY_TOUCH_FRAME_MAX      4096  /* one full report, generous upper bound */

/* Return codes.  Negative on error, 0 on success — same convention as the
 * Novatek protocol layer (nvt_* return codes are checked after every step). */
#define DBY_TOUCH_OK              0
#define DBY_TOUCH_ERR_PARAM     (-22)
#define DBY_TOUCH_ERR_IO        (-5)
#define DBY_TOUCH_ERR_TIMEOUT  (-110)
#define DBY_TOUCH_ERR_PROTO    (-71)
#define DBY_TOUCH_ERR_UNPROBED (-95)  /* protocol layer not implemented yet */

/* Bus abstraction: the driver supplies the GENI SPI transfer and the delay.
 * xfer() must keep chip-select asserted for the whole full-duplex transfer and
 * return DBY_TOUCH_OK only when all len bytes moved. */
struct dby_touch_bus {
	void *ctx;
	int (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
	void (*delay_ms)(void *ctx, unsigned ms);
};

/* What the identify pass learned: the eight bytes the chip returned, which table row they matched,
 * and how many reset/read rounds it took.  Kept as raw bytes too, because the first bring-up run is
 * evidence as much as it is a decision. */
struct dby_touch_identify {
	uint8_t  raw[8];
	int      trim_row;      /* index into the N9Z table, or -1                      */
	unsigned attempts;
};

struct dby_touch_contact {
	int32_t  x;            /* raw coordinates as the controller reported them   */
	int32_t  y;
	int32_t  pressure;     /* 0 .. DBY_TOUCH_FINGER_PRESSURE_MAX                */
	uint8_t  id;
	uint8_t  down;
};

struct dby_touch_frame {
	unsigned count;
	struct dby_touch_contact contact[DBY_TOUCH_MAX_CONTACTS];
};

/* Snapshot with a sequence counter, the mechanism unixnu documents for handing
 * contacts to the HID layer without tearing (docs/zh/drivers/sheng-novatek-touch.md:120-131):
 * the writer bumps the counter before and after the update, the reader retries
 * while the two reads disagree or the counter is odd. */
struct dby_touch_snapshot {
	volatile uint32_t seq;
	volatile uint32_t count;
	volatile int32_t  xs[DBY_TOUCH_MAX_CONTACTS];
	volatile int32_t  ys[DBY_TOUCH_MAX_CONTACTS];
	volatile int32_t  pressure[DBY_TOUCH_MAX_CONTACTS];
	volatile uint32_t ids[DBY_TOUCH_MAX_CONTACTS];
	volatile uint32_t down_mask;
};

/* --- protocol layer: not implemented until the IC handshake is recovered --- */

/* Read-only identify.  Returns DBY_TOUCH_ERR_UNPROBED until the product-id read
 * is implemented; *family_out is set to DBY_TOUCH_IC_FAMILY when non-NULL so the
 * caller can log the expected family. */
int dby_touch_identify(struct dby_touch_bus *bus, const char **family_out);

/* The N9Z pass itself: resets the chip out of bootloader into idle, reads the eight version
 * bytes and matches them against the trim table.  Returns DBY_TOUCH_OK when a row matched. */
int dby_touch_identify_n9z(struct dby_touch_bus *bus, struct dby_touch_identify *out);

/* One full-duplex probe: 0x00 fill, no register assumptions.  Used by stage 3 to
 * prove the bus moves bytes at all before any IC protocol is attempted. */
int dby_touch_bus_probe(struct dby_touch_bus *bus, uint8_t *rx, size_t len);

/* Register access, exported for the board-side chip-enable sequence (stage 6+).  The wire framing
 * is the vendor's: latch the page with {FF, hi, mid}, then a data transfer whose first byte is the
 * low address (| 0x80 for a write).  IMPORTANT: dby_touch_n9z_read returns the RAW transfer, so the
 * chip's two address-phase bytes are at rx[0..1] and the payload starts at rx[2] - callers that want
 * the data must index from 2, exactly as the frame reader and the FWINFO block already do.  Both
 * return DBY_TOUCH_OK or DBY_TOUCH_ERR_*. */
int dby_touch_n9z_write(struct dby_touch_bus *bus, uint32_t addr, uint8_t value);
int dby_touch_n9z_read(struct dby_touch_bus *bus, uint32_t addr, uint8_t *rx, size_t len);
/* Write `len` bytes as one transfer (one page latch, one CS window, address byte first).  Required
 * whenever the bytes form a record the firmware consumes as a unit - the 0x2FE50 mailbox is one. */
int dby_touch_n9z_write_block(struct dby_touch_bus *bus, uint32_t addr, const uint8_t *data, size_t len);

/* Parse one report into contacts.  DBY_TOUCH_ERR_UNPROBED until the layout is
 * known; callers must treat that as "bus works, protocol pending". */
int dby_touch_parse_frame(const uint8_t *buf, size_t len, struct dby_touch_frame *out);

/* --- pure helpers: implemented and host-tested --- */

/* Raw controller units -> panel pixels.  raw = pixel * magnification, so this is
 * raw / 8 for both axes (porting.toml:125 measured 12800x20480 for a
 * 1600x2560 panel; DTBO:10103 says magnification = 8).  Values are clamped to
 * the panel box so a single bad report cannot push a cursor off screen. */
int32_t dby_touch_scale_axis(int32_t raw, unsigned raw_max, unsigned panel_max);

/* Apply the panel rotation.  rotation_kind:
 *   0 none, 1 = 90 deg clockwise, 2 = 180, 3 = 270.
 * Returns 0 on success, DBY_TOUCH_ERR_PARAM for an unknown kind. */
int dby_touch_rotate(int rotation_kind, unsigned panel_w, unsigned panel_h,
                     int32_t *x, int32_t *y);

/* Publish one frame into the snapshot (writer side). */
void dby_touch_snapshot_publish(struct dby_touch_snapshot *snap,
                                const struct dby_touch_frame *frame,
                                int rotation_kind);

/* Read a coherent copy (reader side).  Returns 0 when the copy succeeded within
 * retries, DBY_TOUCH_ERR_TIMEOUT when the writer never settled. */
int dby_touch_snapshot_read(struct dby_touch_snapshot *snap,
                            struct dby_touch_frame *out, unsigned retries);

/* Every N frames the driver logs one line; keep the decision here so the host
 * test can pin the cadence. */
int dby_touch_should_log(uint32_t frame_index);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DBY_TOUCH_PROTOCOL_H */
