/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * DbyTouch.cpp — DBY-W09 touch bring-up driver (I/O side only).
 *
 * WHAT THIS FILE IS
 *   A staged, read-only-by-default driver for the Huawei THP touch node on this
 *   board.  Stages 0..2 are implemented: they map the three register windows,
 *   print a census of the TLMM pins the overlay assigns to touch, decode the
 *   serial-engine identity, and (stage 2 only) apply the two pins the overlay
 *   tells us to apply.  Stages 3..5 need the IC protocol (see DbyTouchProtocol.c)
 *   and a running serial-engine clock, which this driver does not create.
 *
 * WHY IT LOOKS LIKE THIS
 *   - Layout, numbering and gates follow the unixnu input page
 *     (docs/zh/porting/input.md) and the Novatek example it points at
 *     (docs/zh/drivers/sheng-novatek-touch.md): pure core / protocol / board
 *     constants with sources / staged probe / snapshot publishing.
 *   - The MMIO mapping idiom (IODeviceMemory::withRange + kIOMapInhibitCache)
 *     is the one this repository already uses for UFS
 *     (drivers/DbyUfs/NativeUfs.cpp:135-144).
 *   - The TLMM decode (0x0F100000, stride 0x1000, pull/mux/drv/oe bitfields) is
 *     the one already used on this handset by the UEFI key driver
 *     (dby-port/uefi-dby/Platforms/Huawei/dbyPkg/Drivers/DbyKeyInput/DbyKeyInputDxe.c:78-140).
 *
 * WHY THERE IS NO IOHIDDevice HERE
 *   This kernel build defines NO_KERNEL_HID (it is in the compiler record of
 *   osfmk/kern/startup.c) and the XNU work tree has no IOHIDFamily headers at
 *   all.  Touch therefore cannot create an IOHIDDevice inside this archive; it
 *   publishes contacts into the snapshot in DbyTouchProtocol.h, exactly the
 *   split unixnu documents for the HID layer.  See README §2.
 *
 * WHERE THE RESULT IS VISIBLE ON DEVICE
 *   This board has no kernel console after IONVRAM (AGENTS.md), so the boot
 *   progress page is the only place a result can be read back.  That page
 *   already renders a touch row: package/BootProgress.c:571-573 prints
 *   native_touch_rows[0] on row 42 and switches into "touch mode" while
 *   native_touch_stage_req is non-zero, and BootProgressStubs.c:75/88 declares
 *   both weak so a driver may own them.  This file now owns them; both stay
 *   zero (and the page renders exactly what it renders today) unless
 *   dby-touch=<n> was passed.  README §13 is the field-by-field acceptance.
 *
 * 2026-10-29 changes are listed in README §12 with the reason for each.
 */

#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSharedPtr.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>

/* The N9Z firmware image the vendor HAL uploads at every start (generated, 0x30000 bytes).
 * Why it is embedded: this part is a no-flash THP one, so the host has to load it, and the
 * image is part of the closed HAL - there is no file system in this driver to read it from.
 * Provenance: work/tracks/n9z-fw-upload/REPORT.md. */
#include "DbyTouchFw.h"

#include <kern/clock.h>
#include <mach/kern_return.h>
#include <pexpert/pexpert.h>

#include "DbyTouchBoard.h"

/* The protocol layer is plain C; this file is C++, so its entry points are declared here
 * with C linkage to match the definitions in DbyTouchProtocol.c. */
extern "C" {
#include "DbyTouchProtocol.h"
}

/* The official QUPv3 GENI/SPI engine driver.  Plain C, no IOKit: it only needs the register
 * access callbacks below, which is why it can be used from this kext unchanged. */
extern "C" {
#include "GeniSpi.h"
}
#include "DbyTouchProtocol.h"

/* One snapshot for the whole board; the HID layer reads it.
 *
 * STATUS: kept, not written yet.  dby_touch_snapshot_publish() is the writer and
 * stage 5 (frame -> contacts) is what would call it; neither exists, and no HID
 * layer can exist in this build (NO_KERNEL_HID, README §2.2).  It is deliberately
 * not deleted: it is the documented M5/M7 contract
 * (docs/zh/drivers/sheng-novatek-touch.md:120-131), it is the object the pure
 * core is host-tested against, and deleting it would remove the only exercised
 * path of the coordinate code.  Nothing reads or writes it at runtime today, so
 * it costs one .bss object and no instructions. */
struct dby_touch_snapshot gDbyTouchSnapshot;

/* ------------------------------------------------------------------ */
/* Diagnostic page channel                                            */
/* ------------------------------------------------------------------ */

/* These two symbols are the sheng/umi page ABI, not this driver's invention:
 *   char            native_touch_rows[4][120]   BootProgress.c:191/573
 *   volatile unsigned native_touch_stage_req    BootProgress.c:201/571/1297
 * Both are declared weak in package/BootProgressStubs.c:75 and :88 precisely so
 * a touch driver can replace them, and BootProgress guards every use with
 * `&native_touch_rows && native_touch_stage_req` (BootProgress.c:466/571), so a
 * zero value means "no touch this boot" and the page is unchanged.
 *
 * Only the text row is published.  The dot overlay is not: it maps contacts
 * into a fixed 30480 x 20320 space (BootProgress.c:1308/1330) which is the
 * sheng/umi panel's, not this panel's 12800 x 20480, so enabling it would draw
 * dots in the wrong place and look like a driver bug.  See README §13. */
extern "C" {
char native_touch_rows[8][120];
volatile unsigned native_touch_stage_req;

/* Stage-1 pin census results.  IOLog is invisible on this board (no UART), so the per-pin
 * GPIO_CFG words are also published into native_touch_rows[1]; that row is what a photo of the
 * diagnostics page actually shows.  File-scope so the struct layout stays untouched. */
static uint32_t gPinCfg[6];
static unsigned gPinGpio[6];
static unsigned gPinCfgCount;

/* Judgment experiment (2026-10-31): the pin census reads 0x00000000 for all six pins even after the
 * board described the pin controller, so the question is whether the *mapping idiom* fails on this
 * platform or whether that one window carries no hardware. read them once in the entry path (see
 * dby_touch_probe_entry) and published on native_touch_rows[1], i.e. grid row 47, which is the row
 * the log writer archives every two seconds. native_touch_rows[2] would not be archived: the page
 * only captures rows 0..47 and nothing draws row[2]. */
static uint32_t gProbeGic, gProbeUfs, gProbeTlmm;
static uint32_t gProbeGcc;
static uint32_t gScanWestVal;
/* The pin read the firmware performed before XNU started, from boot argument dby-pinprobe. */
static uint32_t gPinProbeFromFirmware;
static bool     gPinProbeSeen;

/* Stage-2 clock vote results (QUPv3 wrapper 2 / SE14), published on native_touch_rows[1]. */
static uint32_t gClkVotePost;
static uint32_t gClkOk, gClkTries;
static bool     gStage2Started;
/* Wrapper-2 branch CBCRs as printed on row 1 (m_ahb, s_ahb, core): a branch that keeps CLK_OFF set
 * proves the vote never reached it. */
static uint32_t gClkHaltMAhb, gClkHaltSAhb, gClkHaltCore, gClkHaltS0, gClkHaltCore2x;
/* The vote-to-branch wiring test: the branch's halt register before, without and with its vote bit. */
static uint32_t gS0Before = 0xFFFFFFFFu, gS0NoVote = 0xFFFFFFFFu, gS0ReVote = 0xFFFFFFFFu;
/* 0x52010 read back after the branch's vote bit was dropped (bit4 == 0 means the clear landed) and
 * 0x52010 as read before the drop, so "nothing to clear" is visible on the page. */
static uint32_t gVoteCleared = 0xFFFFFFFFu, gVoteBefore = 0xFFFFFFFFu;
/* SE root-clock and engine read-backs.  All start at 0xFFFFFFFF, the "not attempted" value, so a
 * skipped step never looks like a register that read zero. */
static uint32_t gSeRcgCmd = 0xFFFFFFFFu;
static uint32_t gSeHwParam0 = 0xFFFFFFFFu, gWrapWord0 = 0xFFFFFFFFu, gSeWord0 = 0xFFFFFFFFu;
static uint32_t gSeWriteBack = 0xFFFFFFFFu;
/* 1 only when the SE's root clock was force-enabled, latched and released with ROOT_OFF clear.
 * The serial engine must not be written unless this is 1: a dead parent RCG still leaves every
 * branch CBCR reading "not off", so the branch poll alone cannot authorise an engine access. */
static uint32_t gSeRcgOk;

/* Stage 3 (GENI SPI bring-up).  Published on row 1, which the log writer archives. */
static struct geni_spi gSpi;
/* 0 = the whole ladder passed, 1..5 = the step that failed, 0xFFFFFFFE = not attempted because the
 * engine verdict was not FIFO-usable, 0xFFFFFFFD = not attempted because the clock gate was closed,
 * 0xFFFFFFFF = the stage-3 thread has not run yet. */
static uint32_t gStage3Rc = 0xFFFFFFFFu;
static uint32_t gStage3Info;               /* protocol<<24 | width<<16 | depth */
static uint32_t gStage3Loop;               /* loopback 4-byte result as a word */
static uint32_t gStage3Irq, gStage3Status;

/* Stage 4: pads muxed, then the chip identify.  gIdentRaw holds the eight bytes the chip returned,
 * two per word, because the first run is evidence as much as it is a decision. */
static uint32_t gStage4Rc = 0xFFFFFFFFu;
static uint32_t gIdentRaw[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
static int      gIdentRow = -1;
static uint32_t gMuxBefore, gMuxAfter;

/* Stage 5: one frame read.  tx[0] = 0x20 asks the chip for its report (no address latch; the vendor
 * uses the same single byte), and the answer is taken in 128-byte pieces with CS held between them
 * because a single long command stops at the RX flow-control watermark.
 *
 * The first two received bytes are command latency, not frame content: the same family reads copy
 * ID bytes from r_buf[i + 2] and its driver defines RX_DATA_START = 2.  Every offset published below
 * is therefore an offset inside the frame, i.e. rx[n + 2]. */
static uint32_t gFrameWord[8] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                  0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
/* Total number of bytes in the answer that are neither 0x00 nor 0xFF (was called gFrameNz,
 * which described an earlier "did two reads differ" experiment that no longer exists). */
static uint32_t gFrameNz = 0xFFFFFFFFu;
/* The length ladder and the engine state at the first failure.  The FIFO path feeds TX in 60-byte
 * batches and waits for the watermark status between them, so the first length that fails tells us
 * whether the batch boundary is the problem; the snapshot words say what the engine was doing. */
static uint32_t gFailLen = 0xFFFFFFFFu;      /* first length that failed, 0 = every length passed */
static uint32_t gFailRc = 0xFFFFFFFFu;
static uint32_t gSnapIrq, gSnapStatus, gSnapTxfs, gSnapRxfs;
/* 0xFFFFFFFF = no release was needed, 1 = the CS-releasing transfer succeeded, 0 = it failed
 * and the chip may be stranded mid-frame until the next power cycle. */
static uint32_t gReleaseOk = 0xFFFFFFFFu;
/* Window offset requested through dby-touch-win, published so a log line says which
 * offset its bytes came from. */
static uint32_t gWindowOff;
/* FWINFO: x_num, y_num and the absolute maxima, read from the event buffer's 0x78 block.  Without
 * these the coordinates cannot be scaled to pixels, so they are read before the frame. */
static uint32_t gGeo[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
/* Two consecutive frames compared on the device: how many bytes differ, and the first one that does
 * with both values. */
/* Many consecutive frames, per-byte min/max over the report area.  A single pair of frames only
 * shows whatever happened to change in that 50 ms window, and a counter qualifies; sampling for a
 * second and taking the byte with the widest range is what finds the part of the frame that follows
 * a finger. */
static uint32_t gLiveOff = 0xFFFFFFFFu;
static uint32_t gLiveMin, gLiveMax;
static uint32_t gLiveMoved;
/* How many offsets moved (range >= 8 across the samples) in each 1 KiB of the payload, saturating at
 * 255 so eight of them fit one row as two hex digits each.  The scan used to stop at 4096 bytes and
 * therefore never looked at the window the frame's payload is now believed to carry the matrix in
 * (work/tracks/n9z-web/REPORT.md: payload = 8192 bytes at rx[2], matrix most likely at 0x1000), so
 * these buckets are what says whether that guess holds on the device. */
static uint32_t gLiveBucket[8];
/* FWINFO answer: the two check bytes, then x_num and y_num, plus whether the check passed. */
static uint32_t gFwByte[8];
static uint32_t gFwValid = 0xFFFFFFFFu;
/* x_num and y_num from the same block: the matrix is 2*x_num*y_num bytes. */
static uint32_t gFwX, gFwY;
/* Frame dump result, published as dp=<ok>@<offset-in-export-region>. */
static uint32_t gDumpRel;
static uint32_t gDumpOk = 0xFFFFFFFFu;

/* The offset of the first byte in the frame that is neither 0x00, 0xFF nor 0x1F fill.  It starts with an
 * idle-looking prefix (FF FF then zeros) at every length measured so far, so the payload has to be
 * located rather than assumed: this is where the next window is taken from. */
static uint32_t gDataOff = 0xFFFFFFFFu;
/* The longest unbroken run of bytes that are neither 0x00, 0xFF nor 0x1F fill.  A floating MISO line gives
 * isolated bytes while real payload arrives in runs, so the length of the longest run is what tells
 * "there is data in this frame" from "there is noise in this frame". */
static uint32_t gDataRun;
/* Where the longest unbroken run starts.  The first non-idle byte is not it: measured runs are short
 * and the first byte belongs to the frame's fixed tail, so the payload window has to be taken from
 * the run itself, not from the first stray byte. */
static uint32_t gRunStart = 0xFFFFFFFFu;
/* Non-idle byte count per 1024-byte block of the scanned 8 KiB payload (log only).  A single "how many
 * bytes carry data" number says nothing about *where* they are; eight counts show the shape of the
 * frame, which is what decides whether the payload sits in a contiguous record area or is scattered
 * noise.  The divisor follows kScanLen: 8192 / 8 blocks. */
static uint32_t gBlockCount[8];
static uint32_t gStage5Rc = 0xFFFFFFFFu;
/* Chip-enable round (stage 6+): whether it ran at all, its return code, what gpio39 did while the
 * window was open, and the raw bytes read at the counter address (unparsed on purpose: the offset
 * is a hypothesis).  Zero everywhere is the honest answer for "the chip is still not scanning", so
 * the row must not hide it. */
static bool     gEnableRan;
static uint32_t gEnableRc = 0xFFFFFFFFu;
static uint32_t gIrqLevel = 0xFFFFFFFFu;
static uint32_t gIrqEdges;
static uint8_t  gCounterBytes[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
/* The identity pass repeated AFTER the reset release.  If the chip came out of hard reset without
 * being taken out of its bootloader, this is the counter that says so - the frame read alone would
 * just return a garbage page. */
static uint32_t gEnableIdentRc = 0xFFFFFFFFu;
static int      gEnableIdentRow = -1;
/* Final pad state of the reset line (CFG and IO registers) after the sequence: the row has to show
 * that the line ended HIGH and muxed to GPIO, because the failure mode this round is guarding
 * against is a line left asserted for the whole boot. */
static uint32_t gResetCfg = 0xFFFFFFFFu;
static uint32_t gResetIo = 0xFFFFFFFFu;
/* Host-command handshake results (stage 7+): one entry per command sent, 0 = the firmware cleared
 * the command byte, 1 = it never did within the retries, 2 = the transfer itself failed.  0xFF means
 * the command was never attempted. */
static uint8_t  gHostCmdRc[3] = { 0xFF, 0xFF, 0xFF };
/* The counter bytes as they read BEFORE the host commands, and whether they moved afterwards.  The
 * second probe overwrites gCounterBytes, so without this copy the pre-command value would be lost
 * and "did the commands change anything" would be unanswerable from the archived row. */
static uint8_t  gCounterFirst[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static bool     gCounterMoved;
/* First byte of the six-byte firmware-reset-state read at 0x2FE60.  0xA0..0xAF means a firmware is
 * up (the vendor HAL's own acceptance range); anything else means the chip has no firmware running,
 * which is what a no-flash THP part looks like before its image is uploaded. */
static uint32_t gFwState = 0xFFFFFFFFu;
/* Firmware upload (stage 8+): result code (0 = the chip reported a booted firmware, 1..29 = the
 * step that stopped), the last 0x2FE60 byte it saw, and how many attempts it took. */
static uint32_t gUploadRc = 0xFFFFFFFFu;
static uint32_t gUploadState = 0xFFFFFFFFu;
static uint32_t gUploadAttempts;
/* Verification read-back, added because three identical failed attempts carried no information:
 * the upload's own "success" only means the SPI transfers completed.  These are READS ONLY - they
 * change no chip state, deliberately, so the first upload after the stream fix is not confounded.
 *   gCrcFlags     0x3F133 & 7 -> bit0 ILM ok, bit1 DLM ok, bit2 CRC done (work/tracks/n9z-upstream)
 *   gCrcMismatch  how many of {0x3F120==0x3F100, 0x3F124==0x3F104} fail: the boot ROM recomputes the
 *                 CRC over what is actually in SRAM, so a mismatch localises "stream never landed"
 *                 versus "SRAM content rejected". */
static uint32_t gCrcFlags = 0xFFFFFFFFu;
static uint32_t gCrcMismatch = 0xFFFFFFFFu;
static uint32_t gAutoCopy = 0xFFFFFFFFu;
/* The four CRC words themselves.  The row has no room for them, and they are what decides between
 * "the write never landed", "0x3F120/0x3F124 are not the ROM's computed CRCs" and "the data really
 * is wrong" - the three readings of an identical c=72.  They are published on grid row 42 when the
 * upload failed, where the clock census (row 42's usual content) is no longer the question. */
static uint32_t gCrcGolden[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
/* Upload progress.  The stream is ~1039 windows of two SPI transfers each, and three attempts, so a
 * boot can spend minutes here; without a counter a slow upload and a hung one look identical from
 * the outside (both leave s5=0xFFFFFFFE "started, not finished").  The counter is published while
 * the upload runs, so the archived page shows movement. */
static uint32_t gUploadWindows;
static uint32_t gUploadWindowsTotal;
/* Bytes that differ between what is in SRAM and what the image says should be there, sampled at the
 * start of each partition.  The golden CRC registers turned out to be writable and readable (they
 * read back exactly 982a680f/688e0af0) while the ROM's computed CRCs differ, so the next question is
 * whether the streamed bytes themselves landed.  Reading SRAM answers it directly instead of
 * inferring through a checksum. */
static uint32_t gSramDiff[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
static uint32_t gSramSampled __attribute__((unused)) = 0xFFFFFFFFu;
static uint32_t gSramFirstBad = 0xFFFFFFFFu;
/* Contact extraction, in kernel.  Calibrated 2026-11-02: matrix is 50 rows x 32 columns (FWINFO's
 * x_num is the COLUMN count), payload at rx[2], 16-bit values around 0x8000 with the matrix starting
 * at payload offset 4, row 0 = top edge, column 0 = left edge.  Pressing a corner moved the affected
 * nodes by 2500..4000 against a noise floor of ~15. */
#define DBY_TOUCH_MATRIX_ROWS 50U
#define DBY_TOUCH_MATRIX_COLS 32U
#define DBY_TOUCH_MATRIX_HDR  4U
/* How far above a node's own reference a contact has to be.  Measured over the 40-frame capture
 * (evidence/edl-frames40/frames40.img): the largest node excursion in a quiet frame is 580, the
 * largest in a press 4161, so the ceiling is 1.9x above the strongest measured press.  A node above
 * the ceiling is CLAMPED, not dropped: a contact can be three nodes wide, so dropping one node's
 * weight can drop the whole contact.
 *
 * 2026-11-03: floor lowered 1200 -> 700 for sensitivity.  A light touch raises its centre node by
 * about 1500 and its neighbours by 500..1700, so at 1200 only one or two nodes cleared the floor and
 * the DBY_TOUCH_MIN_NODES gate below then rejected the whole touch - which is the "it works but it is
 * not sensitive" report.  700 keeps margin over every quiet-frame excursion measured in a PRESS
 * recording (580 in the 20-frame sweep, 468 in the 00:35 boot), but NOT over an untouched boot: review
 * M1 showed the 791 s recording puts 2..4 nodes above 700 in 13 of 39 frames when a reference step lands
 * at frame 10, and the node gate does not stop that - a 2-node pair peaked at 774.  What stops it is
 * DBY_TOUCH_TOUCH_PEAK below, which is why that gate must never be removed on the strength of this
 * floor alone. */
#define DBY_TOUCH_TOUCH_FLOOR   700U
#define DBY_TOUCH_TOUCH_CEILING 8000U
/* Two thresholds, one job each.  The floor above decides how much of the finger's skirt joins the
 * centroid - that is what fixes sensitivity and smoothness.  This one decides whether there is a
 * finger at all: at least one node of the winning component must reach it.
 *
 * Proven offline with work/verify/emulate-connected.py over the three recordings.  At floor 700 the
 * untouched 791 s boot (evidence/edl-frames40-2230) produced 14 contacts with a flat sum and still 1
 * with connectivity alone (2 adjacent nodes, peak 774); adding this gate gives 0.  The four-corner
 * recording keeps 18 of 39 contacts, unchanged.  A real light press peaks near 1500, so 1200 keeps
 * 1.25x margin over it while sitting 1.55x above that residual phantom. */
#define DBY_TOUCH_TOUCH_PEAK    1200U
/* Node-count bounds: a finger covers 3..80 nodes; hundreds means a palm or a drifting reference.
 * DBY_TOUCH_CONTACT_NONE separates "the reader ran and found nothing" (0xFFFFFFFE, i.e. a lift) from
 * "not run yet" (0xFFFFFFFF).  The panel size is NOT redeclared here: it lives in DbyTouchBoard.h
 * (DBY_TOUCH_PANEL_W/H, device tree W09_VA 1600x2560), and a second copy with the same value
 * would compile silently and drift later.
 *
 * MIN_NODES 3 -> 2 on 2026-11-03, together with the floor.  The earlier justification - "two nodes above
 * 700 cannot be produced by a quiet frame" - is FALSE and was refuted by review M1: the untouched 791 s
 * recording produces exactly that, a 2-node ADJACENT pair peaking at 774.  2 is therefore only safe
 * together with the other two gates that came with it: the 4-connected component selection above and
 * DBY_TOUCH_TOUCH_PEAK, which that pair fails (774 < 1200).  Removing either of those re-opens the
 * phantom. */
#define DBY_TOUCH_MIN_NODES   2U
#define DBY_TOUCH_MAX_NODES   80U
#define DBY_TOUCH_CONTACT_NONE 0xFFFFFFFEu
/* A drag must not be cut in half by one weak frame.  When a frame fell under the node gate the reader
 * published a lift immediately, so a finger that lightened for a single frame ended the gesture -
 * the other half of "gestures are unstable".  The last contact now stands for up to this many
 * consecutive empty frames before a lift is published; at the 2026-11-03 sample period that is
 * ~120 ms of hysteresis, which is below the threshold where a real lift feels late. */
#define DBY_TOUCH_LIFT_DEBOUNCE 3U

static uint32_t gTouchPixels[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
static uint32_t gTouchWeight;
static uint32_t gTouchNodes;
/* Lift hysteresis (DBY_TOUCH_LIFT_DEBOUNCE): consecutive frames that failed the node gate, and
 * whether a contact is currently held through them. */
static unsigned gTouchMissRun;
static unsigned gTouchHeld;
/* Frames whose SPI read failed, i.e. frames that never reached the reader.  Published on row 42 so
 * a later readback can tell "the reader is slow" from "the reader keeps missing frames". */
static uint32_t gTouchReadFails;

/* ---- multi-contact tracking (2026-11-03 17:40, track A3) ----
 *
 * Until this change the reader published exactly one contact: it flood-selected the heaviest component
 * and wrote native_touch_count = 1.  Pinch-zoom and three-finger gestures cannot exist on top of that,
 * so a frame now yields up to DBY_TOUCH_MAX_CONTACTS components (10, the protocol header's cap and the
 * HID consumer's TOUCH_FINGERS limit) and each one keeps a stable id while it stays down.
 *
 * The per-component gates are unchanged - 4-connectivity, node bounds, DBY_TOUCH_TOUCH_PEAK, weight by
 * (delta - floor) - because they were measured (see the long comments above).  Multi-touch changes how
 * many components survive, not how one of them is judged.
 */
/* The contact cap is NOT redeclared here: DbyTouchProtocol.h:44 already defines
 * DBY_TOUCH_MAX_CONTACTS as 10, which is also the HID consumer's TOUCH_FINGERS limit
 * (NativeHidInput.cpp), and the native_touch_xs/ys/ids arrays are 12 wide, so 10 fits with room to
 * spare.  A second, larger copy here would have compiled silently and then disagreed with the ABI. */
#define DBY_TOUCH_MAX_CANDIDATES 24U  /* components per frame worth keeping, heaviest first */
/* Largest per-frame step treated as motion, in panel pixels: it bounds the velocity used to predict a
 * contact's next position, so one bad centroid cannot fling the prediction across the panel.  It is NOT
 * a matching threshold any more - an incoming component is always paired with its nearest free contact
 * (see the comment at the matcher), because a threshold turned fast swipes into phantom fingers. */
#define DBY_TOUCH_MATCH_PX       220U

struct DbyTouchContact {
	uint32_t x, y;      /* panel pixels, last confirmed */
	int32_t vx, vy;     /* per-frame velocity, used to predict the next position */
	uint32_t weight;
	uint32_t id;        /* stable while the finger stays down; 1..255 because the report packs 8 bits */
	uint16_t nodes;
	uint8_t miss;       /* consecutive frames this id was not seen */
	uint8_t live;       /* 1 = publish this frame */
};

static struct DbyTouchContact gTouchContacts[DBY_TOUCH_MAX_CONTACTS];
static uint32_t gTouchLiveCount;      /* contacts published in the last frame */
/* Ids come from dbyTouchNextFreeId() rather than a counter: a monotonic counter would alias two
 * fingers once it passed 255, because the HID report packs the identifier into one byte. */

struct DbyTouchCandidate {
	unsigned seed;
	unsigned nodes;
	uint64_t weight;
	uint32_t peak;
};

static struct DbyTouchCandidate gTouchCand[DBY_TOUCH_MAX_CANDIDATES];
static unsigned gTouchCandCount;
static uint32_t gCrcComputed[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };

/* ------------------------------------------------------------------ */
/* native_touch_* producer (the seam the OS input layer reads)         */
/* ------------------------------------------------------------------ */
/*
 * The OS side of touch is not this board's code.  Whenever the ShengHidInput device is linked it
 * turns the five globals below into absolute digitizer reports and hands them to the stock
 * IOHIDEventDriver (NativeHidInput.cpp:155-206), and the boot page already draws a dot overlay from
 * the same five globals (BootProgress.c:1166-1181).  Both read them over this seqlock, so the
 * producer below is the whole contract - the same one sheng's touch core fills
 * (work/refs/sheng/TouchProbe.cpp:340-358).
 *
 * uint32_t for the sequence and the count because both readers declare them that way
 * (NativeHidInput.cpp:18, BootProgress.c:114); the weak stub in package/BootProgressStubs.c said
 * uint64_t and is corrected in this change.
 */
volatile uint32_t native_touch_seq;
volatile uint32_t native_touch_count;
volatile int native_touch_xs[12];
volatile int native_touch_ys[12];
volatile int native_touch_ids[12];

/* Axis maxima of the domain the consumers scale by.  Both hardcode this pair
 * (NativeHidInput.cpp:172, BootProgress.c:1175), so the producer has to publish in it, not in pixels. */
#define DBY_TOUCH_HID_LONG_AXIS  30480
#define DBY_TOUCH_HID_SHORT_AXIS 20320

static void (*gTouchSink)(void *);
static void *gTouchSinkContext;
static uint32_t gTouchPublishes;
/* Reader bookkeeping for the diagnostics row: how many frames the reader has looked at and how many
 * of them held a contact.  Declared here rather than next to gTouchBase because publishPageRow(),
 * which reports them, is defined above that block. */
static uint32_t gTouchFrames;
static uint32_t gTouchHits;

/* The consumer registers here once, at start().  The HID archive ships a weak empty definition
 * (DbyHidSinkStubs.c:30-35) so it links without a producer; this strong one replaces it. */
extern "C" void
native_touch_set_sink(void (*sink)(void *), void *context)
{
	gTouchSinkContext = context;
	__atomic_store_n(&gTouchSink, sink, __ATOMIC_RELEASE);
}

static int
dbyTouchClamp(int v, int limit)
{
	if (v < 0) {
		return 0;
	}
	return v > limit - 1 ? limit - 1 : v;
}

static bool     gStage2Done;
/* SPMI arbiter facts for the diagnostics row: the generation, how many APIDs the map holds, whether
 * PPID 0x71 (the PM8550/SM8550 SDAM PPID sheng uses) is present, and the first non-zero PPID found.
 * Published because this board answered rc=4 (no SDAM APID) with sheng's PPID, so the value has to
 * come from the table rather than from a ported constant. */
uint32_t dby_spmi_version, dby_spmi_apids, dby_spmi_has071, dby_spmi_first_ppid;
/* SPMI arbiter discovery, published on native_touch_rows[1]: the version it reported, how many
 * APIDs it carries, and the PPIDs of the first few - enough to find the SDAM peripheral by eye
 * instead of guessing which PPID this PMIC uses (sheng's 0x071 is SM8550's). */
static uint32_t gSpmiVersion, gSpmiApidCount, gSpmiPpid[6], gSpmiPpidCount;
/* A probe that could not map its window must not look like a register that read zero. */
#define DBY_TOUCH_PROBE_FAIL 0xFFFFFFFFU
}

namespace {

/* ------------------------------------------------------------------ */
/* MMIO                                                               */
/* ------------------------------------------------------------------ */

struct DbyTouchMap {
	OSSharedPtr<IOMemoryMap> keep;
	uintptr_t base;
	uint64_t  length;          /* as mapped; every access below is bounds-checked
	                            * against it instead of against a constant */
};

bool
mapPhys(uint64_t phys, uint64_t length, DbyTouchMap *out)
{
	IODeviceMemory *memory;

	if (out == nullptr) {
		return false;
	}
	out->base = 0;
	out->length = 0;
	memory = IODeviceMemory::withRange(phys, length);
	if (memory == nullptr) {
		return false;
	}
	out->keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache),
	                                     OSNoRetain);
	memory->release();
	if (!out->keep) {
		return false;
	}
	out->base = static_cast<uintptr_t>(out->keep->getVirtualAddress());
	out->length = (out->base != 0) ? length : 0;
	return out->base != 0;
}

/* Drop a mapping *and* its base address.  Resetting only the OSSharedPtr left a
 * non-zero base behind, and mapWindows() skips a window whose base is already
 * set — so a stop()/start() cycle would have kept using an unmapped pointer. */
void
unmapWindow(DbyTouchMap *out)
{
	if (out == nullptr) {
		return;
	}
	out->keep.reset();
	out->base = 0;
	out->length = 0;
}

inline uint32_t
rd32(uintptr_t base, uint64_t offset)
{
	return *reinterpret_cast<volatile uint32_t *>(base + static_cast<uintptr_t>(offset));
}

/* Every write on this board is read back.  dby-port/uefi-dby/.../DbyKeyInputDxe.c:327-330
 * documents that some MMIO written here is swallowed in place (APSS watchdog,
 * SMMU), so "the store executed" is never taken as "the value landed". */
bool
wr32Checked(uintptr_t base, uint64_t offset, uint32_t value, uint32_t mask)
{
	uint32_t back;

	*reinterpret_cast<volatile uint32_t *>(base + static_cast<uintptr_t>(offset)) = value;
	__asm__ volatile("dsb sy" ::: "memory");
	back = rd32(base, offset);
	return (back & mask) == (value & mask);
}

/* ------------------------------------------------------------------ */
/* Row formatting for the page                                        */
/* ------------------------------------------------------------------ */

/* snprintf's prototype is not reachable from the headers this translation unit
 * includes (no driver in this tree calls it), and a wrong declaration in kernel
 * code is a silent stack corruption, so the one diagnostic row is assembled
 * here with nothing but pointer arithmetic.  Every helper keeps the buffer
 * NUL-terminated and never writes past cap-1. */
unsigned
rowAppend(char *out, unsigned cap, unsigned at, const char *text)
{
	while (out != nullptr && text != nullptr && *text != '\0' && at + 1U < cap) {
		out[at++] = *text++;
	}
	if (out != nullptr && cap != 0 && at < cap) {
		out[at] = '\0';
	}
	return at;
}

unsigned
rowAppendDec(char *out, unsigned cap, unsigned at, uint32_t value)
{
	char tmp[10];                       /* 2^32-1 is ten decimal digits */
	unsigned n = 0;

	if (out == nullptr) {               /* same guard as rowAppend(): the three
	                                     * helpers are called in a chain and a
	                                     * NULL row must not become a fault */
		return at;
	}
	do {
		tmp[n++] = static_cast<char>('0' + (value % 10U));
		value /= 10U;
	} while (value != 0U && n < sizeof(tmp));
	while (n > 0 && at + 1U < cap) {
		out[at++] = tmp[--n];
	}
	if (cap != 0 && at < cap) {
		out[at] = '\0';
	}
	return at;
}


unsigned
rowAppendHex8(char *out, unsigned cap, unsigned at, uint32_t value)
{
	static const char digits[] = "0123456789abcdef";
	unsigned shift = 28U;

	if (out == nullptr) {
		return at;
	}
	for (;;) {
		if (at + 1U < cap) {
			out[at++] = digits[(value >> shift) & 0xFU];
		}
		if (shift == 0U) {
			break;
		}
		shift -= 4U;
	}
	if (cap != 0 && at < cap) {
		out[at] = '\0';
	}
	return at;
}

/* Two hex digits: one 1 KiB bucket count of gLiveBucket[]. */
unsigned
rowAppendHex2(char *out, unsigned cap, unsigned at, uint32_t value)
{
	static const char digits[] = "0123456789abcdef";
	unsigned shift = 4U;

	if (out == nullptr) {
		return at;
	}
	for (;;) {
		if (at + 1U < cap) {
			out[at++] = digits[(value >> shift) & 0xFU];
		}
		if (shift == 0U) {
			break;
		}
		shift -= 4U;
	}
	if (cap != 0 && at < cap) {
		out[at] = '\0';
	}
	return at;
}

/* ------------------------------------------------------------------ */
/* Board state                                                        */
/* ------------------------------------------------------------------ */

/* Serial-engine census verdicts (order is the table in the page row). */
enum {
	kSeVerdictUnknown = 0,      /* SE window not mapped, or census not run */
	kSeVerdictUnclocked = 1,    /* identity words read as 0 / all-ones      */
	kSeVerdictNotSpi = 2,       /* GENI protocol field is not SPI           */
	kSeVerdictFifoDisabled = 3, /* SPI, but the FIFO interface is off       */
	kSeVerdictFifoUsable = 4    /* SPI + FIFO: a transfer could be tried    */
};

struct DbyTouchState {
	/* One mapping per pin-controller tile: a pin's registers live at tile_base + pin * stride. */
	DbyTouchMap tlmm;        /* west  */
	DbyTouchMap tlmmSouth;
	DbyTouchMap tlmmNorth;
	DbyTouchMap se;
	DbyTouchMap qup;
	DbyTouchMap gcc;
	uint32_t stage;
	bool censusLogged;
	/* Identity words kept for the page row (see the header comment). */
	uint32_t seStatus;
	uint32_t seFwRev;
	uint32_t seHwParam0;
	uint32_t seIfDisable;
	uint32_t qupVersion;
	unsigned seVerdict;
	unsigned pinWrites;
	unsigned pinWriteFailures;
};

DbyTouchState gState;

/* The TLMM window is sized from the highest pin this driver uses.  If a future
 * edit raises one pin above DBY_TOUCH_GPIO_RESET the window would silently stop
 * covering it; this makes that a compile error instead of a stray MMIO access. */
static_assert(DBY_TOUCH_GPIO_RESET >= DBY_TOUCH_GPIO_IRQ &&
    DBY_TOUCH_GPIO_RESET >= DBY_TOUCH_GPIO_CS &&
    DBY_TOUCH_GPIO_RESET >= DBY_TOUCH_GPIO_SPI_0 &&
    DBY_TOUCH_GPIO_RESET >= DBY_TOUCH_GPIO_SPI_1 &&
    DBY_TOUCH_GPIO_RESET >= DBY_TOUCH_GPIO_SPI_2,
    "DBY_TOUCH_GPIO_RESET sizes the TLMM mapping: every touch pin must be at or below it");

/* Register block offset for one TLMM pin, or false when the pin lies outside
 * the window that was actually mapped.  Computing the offset anyway would touch
 * an unrelated physical page; on this board an unrelated MMIO page is exactly
 * what stalled the UEFI key survey (DbyKeyInputDxe.c:107-113). */
/* The pin controller is tiled on SM8250: 39/40/41/42/43 live in the south window and 98 in the
 * north one, so the register of a pin is its tile's base plus pin * stride.  Returns nullptr when
 * that tile was not mapped; every caller treats that as "cannot touch this pin". */
DbyTouchMap *
tlmmTile(unsigned pin)
{
	switch (DBY_TOUCH_TILE(pin)) {
	case DBY_TOUCH_TILE_SOUTH:
		return (gState.tlmmSouth.base != 0) ? &gState.tlmmSouth : nullptr;
	case DBY_TOUCH_TILE_NORTH:
		return (gState.tlmmNorth.base != 0) ? &gState.tlmmNorth : nullptr;
	case DBY_TOUCH_TILE_WEST:
		return (gState.tlmm.base != 0) ? &gState.tlmm : nullptr;
	case DBY_TOUCH_TILE_NONE:
	default:
		return nullptr;
	}
}

bool
tlmmBlock(unsigned pin, uint64_t *out)
{
	const uint64_t span = 4ULL * sizeof(uint32_t);   /* CFG, IN, INTR_CFG, INTR_STS */
	DbyTouchMap *tile;
	uint64_t block;

	if (out == nullptr) {
		return false;
	}
	tile = tlmmTile(pin);
	if (tile == nullptr || tile->length == 0) {
		return false;
	}
	block = static_cast<uint64_t>(pin) * DBY_TOUCH_TLMM_STRIDE;
	if (block > tile->length || (tile->length - block) < span) {
		return false;
	}
	*out = block;
	return true;
}

const char *
seVerdictText(unsigned verdict)
{
	static const char *const names[] = {
		"UNKNOWN", "UNCLOCKED", "NOT-SPI", "FIFO-DISABLED", "FIFO-USABLE"
	};

	if (verdict >= (sizeof(names) / sizeof(names[0]))) {
		return names[0];
	}
	return names[verdict];
}

/* Row 42 of the boot progress page.  One line, no MMIO, no locks: the callers already hold the
 * values.  Worst case is
 * "TOUCH st=2 rcg=ffffffff rok=1 hw0=ffffffff wb=ffffffff pw=99/99 se=FIFO-DISABLED fw=ffffffff qp=ffffffff sst=ffffffff",
 * which is 111 of the 120 bytes the row has. */
/* Refresh the published snapshot for one pin from the hardware. */
void
pinCfgRefresh(unsigned pin, uint32_t value)
{
	for (unsigned i = 0; i < gPinCfgCount && i < 6; i++) {
		if (gPinGpio[i] == pin) {
			gPinCfg[i] = value;
			return;
		}
	}
}

/* The one-off window probe of 2026-10-31 (GIC/UFS/TLMM/GCC) is no longer published: its numbers are
 * in evidence/TOUCH-BRINGUP-2026-10-31.md and the pair of live canaries (pins, engine) replaced it.
 * Keeping the values referenced stops -Wunused-but-set-global from turning them into a build error,
 * while the code that produced them stays as the record of how the tile mistake was found. */
static void dbyProbeKeepAlive(void)
{
	(void)gProbeGic; (void)gProbeUfs; (void)gProbeTlmm; (void)gProbeGcc; (void)gScanWestVal;
	(void)gSpmiVersion; (void)gSpmiApidCount; (void)gSpmiPpid; (void)gSpmiPpidCount;
	/* Row 1 currently carries the vote-to-branch wiring test, so the firmware cross-check and the
	 * other halted words are not printed this round; they stay referenced because they are the
	 * evidence trail for what each round measured. */
	(void)gPinProbeFromFirmware; (void)gPinProbeSeen;
	/* Values that are measured but not printed this round: referenced so the compiler does not
	 * discard them, each on its own line so no source line runs past the row width. */
	(void)gClkHaltMAhb;
	(void)gVoteCleared;
	(void)gVoteBefore;
	(void)gStage3Rc;
	(void)gStage4Rc;
	(void)gIdentRaw[0];
	(void)gIdentRow;
	(void)gMuxAfter;
	(void)gStage3Loop;
	(void)gClkHaltSAhb;
	(void)gClkHaltCore;
	(void)gClkHaltS0;
	(void)gClkHaltCore2x;
	(void)gS0Before;
	(void)gS0NoVote;
	(void)gStage3Irq;
	(void)gStage3Status;
	(void)gMuxBefore;
	(void)gFrameWord[0];
	(void)gFrameWord[2];
	(void)gFrameWord[3];
	(void)gGeo[0];
	(void)gLiveMin;
	(void)gLiveMax;
	(void)gLiveMoved;
	(void)gFrameNz;
	(void)gFailRc;
	(void)gFailLen;
	(void)gSnapStatus;
}

void
publishPageRow(void)
{
	char *row = native_touch_rows[0];
	const unsigned cap = sizeof(native_touch_rows[0]);
	unsigned at = 0;

	/* One line, no MMIO: it reports what the drivers already measured.
	 *
	 * The fields are the ones that decide the next step, in the order the bring-up ladder reaches
	 * them: the clock verdict (rcg/rok), whether the engine window answers at all (w0/s0/wb), and
	 * the engine identity (hw0) plus the wrapper version (qp).  s0= is GENI_INIT_CFG_REVISION, a
	 * read-only silicon constant: if even that reads zero the window is not responding.
	 *
	 * Worst case, with every word 0xffffffff and se=FIFO-DISABLED:
	 * "TOUCH st=2 rcg=ffffffff rok=1 hw0=ffffffff w0=ffffffff s0=ffffffff wb=ffffffff pw=99/99 qp=ffffffff se=FIFO-DISABLED"
	 * is 118 of the 120 bytes the row has. */
	dbyProbeKeepAlive();
	/* An upload that failed is the round's question, and its four CRC words do not fit in row 47.
	 * Row 42 is archived too (grid row 42 < 48), so it carries them instead of the clock census
	 * for that boot. */
	if (gTouchLiveCount != 0u && gTouchPixels[0] != 0xFFFFFFFFu &&
	    gTouchPixels[0] != DBY_TOUCH_CONTACT_NONE) {
		/* A contact found in the frame just dumped: report it in pixels, prefixed with the live contact
	 * count because the row is the only on-device evidence of how many fingers are being published. */
		at = rowAppend(row, cap, at, "TX c=");
		at = rowAppendDec(row, cap, at, gTouchLiveCount);
		at = rowAppend(row, cap, at, " ");
		at = rowAppendDec(row, cap, at, gTouchPixels[0]);
		at = rowAppend(row, cap, at, ",");
		at = rowAppendDec(row, cap, at, gTouchPixels[1]);
		at = rowAppend(row, cap, at, " w=");
		at = rowAppendDec(row, cap, at, gTouchWeight);
		at = rowAppend(row, cap, at, " n=");
		at = rowAppendDec(row, cap, at, gTouchNodes);
		at = rowAppend(row, cap, at, " hid=");
		at = rowAppendDec(row, cap, at, gTouchPublishes);
		at = rowAppend(row, cap, at, "/");
		at = rowAppendDec(row, cap, at, gTouchSink != nullptr ? 1u : 0u);
		(void)at;
		return;
	}
	/* Third state, and it has to be its own line: "the reader ran and found nothing" is a LIFT
	 * (0xFFFFFFFE), which with latest-wins is one frame away at any moment - and in the recorded
	 * sweep it is literally the last frame.  Printing the ordinary TOUCH line here would make a lift
	 * and a reader that never ran look identical, which is what review #3 (D2) rejected. */
	if (gTouchFrames != 0u) {
		at = rowAppend(row, cap, at, "TX none f=");
		at = rowAppendDec(row, cap, at, gTouchFrames);
		at = rowAppend(row, cap, at, " h=");
		at = rowAppendDec(row, cap, at, gTouchHits);
		/* x= counts frames whose SPI read failed, i.e. frames the reader never saw.  Without it a
		 * low frame rate cannot be told apart from a lossy one: f= only counts what arrived. */
		at = rowAppend(row, cap, at, " x=");
		at = rowAppendDec(row, cap, at, gTouchReadFails);
		at = rowAppend(row, cap, at, " hid=");
		at = rowAppendDec(row, cap, at, gTouchPublishes);
		at = rowAppend(row, cap, at, "/");
		at = rowAppendDec(row, cap, at, gTouchSink != nullptr ? 1u : 0u);
		(void)at;
		return;
	}
	if (gUploadRc == 0xFFFFFFFFu && gUploadWindows) {
		/* Upload in flight: a progress line, so "slow" and "hung" are distinguishable. */
		at = rowAppend(row, cap, at, "UP w=");
		at = rowAppendDec(row, cap, at, gUploadWindows);
		at = rowAppend(row, cap, at, "/");
		at = rowAppendDec(row, cap, at, gUploadWindowsTotal);
		at = rowAppend(row, cap, at, " a=");
		at = rowAppendDec(row, cap, at, gUploadAttempts);
		(void)at;
		return;
	}
	if (gUploadRc != 0xFFFFFFFFu && gUploadRc != 0u) {
		at = rowAppend(row, cap, at, "UP g=");
		at = rowAppendHex8(row, cap, at, gCrcGolden[0]);
		at = rowAppend(row, cap, at, " c=");
		at = rowAppendHex8(row, cap, at, gCrcComputed[0]);
		at = rowAppend(row, cap, at, " d=");
		at = rowAppendHex8(row, cap, at, gCrcGolden[1]);
		at = rowAppend(row, cap, at, " e=");
		at = rowAppendHex8(row, cap, at, gCrcComputed[1]);
		at = rowAppend(row, cap, at, " ac=");
		if (gAutoCopy == 0xFFFFFFFFu) {
			at = rowAppend(row, cap, at, "-");
		} else {
			at = rowAppendHex2(row, cap, at, gAutoCopy);
		}
		/* m=<ilm diffs>/<dlm diffs> over the first 30 payload bytes of each partition. */
		at = rowAppend(row, cap, at, " m=");
		for (unsigned k = 0; k < 2; k++) {
			if (gSramDiff[k] == 0xFFFFFFFFu) {
				at = rowAppend(row, cap, at, "-");
			} else {
				at = rowAppendDec(row, cap, at, gSramDiff[k]);
			}
			if (k == 0) {
				at = rowAppend(row, cap, at, "/");
			}
		}
		/* o= the offset of the first wrong byte in the ILM sample: 0 points at the address byte or
		 * the latch, a larger value points at where the stream starts drifting. */
		at = rowAppend(row, cap, at, " o=");
		if (gSramFirstBad == 0xFFFFFFFFu) {
			at = rowAppend(row, cap, at, "-");
		} else {
			at = rowAppendDec(row, cap, at, gSramFirstBad);
		}
		(void)at;
		return;
	}
	at = rowAppend(row, cap, at, "TOUCH st=");
	at = rowAppendDec(row, cap, at, gState.stage);
	at = rowAppend(row, cap, at, " rcg=");
	at = rowAppendHex8(row, cap, at, gSeRcgCmd);
	at = rowAppend(row, cap, at, " rok=");
	if (!gStage2Done) {
		/* '-' while the stage-2 thread has not reached the clock step; 0 would claim a failure
		 * that was never attempted. */
		at = rowAppend(row, cap, at, "-");
	} else {
		at = rowAppendDec(row, cap, at, gSeRcgOk);
	}
	at = rowAppend(row, cap, at, " hw0=");
	at = rowAppendHex8(row, cap, at, gSeHwParam0);
	at = rowAppend(row, cap, at, " w0=");
	at = rowAppendHex8(row, cap, at, gWrapWord0);
	at = rowAppend(row, cap, at, " s0=");
	at = rowAppendHex8(row, cap, at, gSeWord0);
	at = rowAppend(row, cap, at, " wb=");
	at = rowAppendHex8(row, cap, at, gSeWriteBack);
	at = rowAppend(row, cap, at, " pw=");
	at = rowAppendDec(row, cap, at, gState.pinWrites);
	at = rowAppend(row, cap, at, "/");
	at = rowAppendDec(row, cap, at, gState.pinWriteFailures);
	at = rowAppend(row, cap, at, " qp=");
	at = rowAppendHex8(row, cap, at, gState.qupVersion);
	at = rowAppend(row, cap, at, " se=");
	at = rowAppend(row, cap, at, seVerdictText(gState.seVerdict));
	(void)at;

	/* Row 1 goes to grid row 47, which the log writer archives every two seconds.  It carries the
	 * vote-to-branch wiring test.  vc is 0x52010 read back with the branch's vote bit dropped, so
	 * the clear's success is visible; s0a/s0b/s0c are gcc_qupv3_wrap2_s0_clk's halt register before,
	 * without and after restoring the vote bit.  The judgement is A against B, not A against C:
	 * s0b != s0a proves the wiring, s0b == s0a with bit4 actually cleared means the vote bit does
	 * not drive that status register, and s0b == s0a with bit4 still set means nothing was tested.
	 * v0 is the vote before the clear: if its bit4 is already 0 there was nothing to clear and the
	 * round says nothing at all. */
	{
		char *row2 = native_touch_rows[1];
		const unsigned cap2 = sizeof(native_touch_rows[1]);
		unsigned at2 = 0;

		/* Stage 3 first: this is the row the next decision is made from.  The clock fields stay
		 * behind it because a failing transfer is almost always a clock story. */
		/* Stage 4 first: the identify answer is what the next decision hangs on.  s3/lp stay
		 * because a silent chip is almost always a transport story. */
		/* The frame first: with the chip identified, the open question is what it actually sends.
		 * id/row stay because a frame means nothing if the chip is the wrong one. */
		/* The successful shape: where the first non-idle byte is, how long the longest unbroken run
		 * is and where it starts, the first two words of that run, and a per-64-byte map of how many
		 * bytes in each block carry data.  That is enough to tell a record area from noise, and to
		 * pick the next window without another round trip. */
		/* Two shapes, each sized to fit: a successful read publishes both windows, a failed one
		 * publishes how far it got and the engine's state.  The failure shape used to include the
		 * windows too and was cut off by the row width, which made the one line that mattered read
		 * as if rx were zero. */
		at2 = rowAppend(row2, cap2, at2, "s5=");
		at2 = rowAppendHex8(row2, cap2, at2, gStage5Rc);
		if (gStage5Rc == 0) {
			/* FWINFO is the decisive measurement: it names the matrix dimensions, so it leads the
			 * row.  dp says whether the frame dump happened, and the id confirms the chip. */
			at2 = rowAppend(row2, cap2, at2, " fw=");
			if (gFwValid == 0xFFFFFFFFu) {
				at2 = rowAppend(row2, cap2, at2, "-");
			} else {
				/* The two bytes the validity test adds up, and nothing more: the remaining six are
				 * still published raw in gFwByte[], but the row is 119 bytes and the payload buckets
				 * below are the measurement this round is actually about. */
				at2 = rowAppendDec(row2, cap2, at2, gFwByte[0] & 0xFFu);
				at2 = rowAppend(row2, cap2, at2, ",");
				at2 = rowAppendDec(row2, cap2, at2, gFwByte[1] & 0xFFu);
			}
			at2 = rowAppend(row2, cap2, at2, " ok=");
			if (gFwValid == 0xFFFFFFFFu) {
				at2 = rowAppend(row2, cap2, at2, "-");
			} else {
				at2 = rowAppendDec(row2, cap2, at2, gFwValid);
			}
			/* xy = the two dimensions the block claims, printed only when the check pair added up.
			 * A successful read whose check failed still leaves meaningful-looking bytes at rx[4] and
			 * rx[5], and "ok=0 xy=40,60" would read as a geometry claim the same row just rejected. */
			at2 = rowAppend(row2, cap2, at2, " xy=");
			if (gFwValid != 1u) {
				at2 = rowAppend(row2, cap2, at2, "-");
			} else {
				at2 = rowAppendDec(row2, cap2, at2, gFwX);
				at2 = rowAppend(row2, cap2, at2, ",");
				at2 = rowAppendDec(row2, cap2, at2, gFwY);
			}
			/* lv= is the "longest live window" probe.  It is dropped once the enable round runs:
			 * every stage-6 sample read lv=4294967295 (no window found at all) because the frame is
			 * now a uniform block, and its 13 bytes are worth more to the enable/host-command
			 * fields than to a probe that no longer resolves anything. */
			if (!gEnableRan) {
				at2 = rowAppend(row2, cap2, at2, " lv=");
				at2 = rowAppendDec(row2, cap2, at2, gLiveOff);
			}
			/* vb = one two-digit hex count per 1 KiB of payload: how many offsets in that KiB
			 * changed by 8 or more across the eight samples, saturating at ff.  Expect vbv to read
			 * 0000000012... if the matrix really sits at payload 0x1000. */
			/* vb= is the eight-sample variation count.  It is dropped once the enable round runs:
			 * every stage-6 sample was 0000000000000000 (the frame is a stable block), so its 19
			 * bytes are better spent on fb= and hc=, which are this round's actual questions. */
			if (!gEnableRan) {
				at2 = rowAppend(row2, cap2, at2, " vb=");
				for (unsigned i = 0; i < 8; i++) {
					at2 = rowAppendHex2(row2, cap2, at2, gLiveBucket[i]);
				}
			}
			at2 = rowAppend(row2, cap2, at2, " dp=");
			if (gDumpOk == 0xFFFFFFFFu) {
				at2 = rowAppend(row2, cap2, at2, "-");
			} else {
				at2 = rowAppendDec(row2, cap2, at2, gDumpOk);
				at2 = rowAppend(row2, cap2, at2, "@");
				at2 = rowAppendDec(row2, cap2, at2, gDumpRel);
			}
			/* The chip-enable fields are NOT printed here: this branch only runs when the frame
			 * read succeeded, and a failed enable is exactly the case where they matter most.
			 * They are appended after the if/else chain, on every branch. */
		} else if (gStage5Rc == 0xFFFFFFFEu) {
			/* Started, still working: the frame counter and the engine registers have not been
			 * touched yet, so printing them would read as "measured zero". */
			at2 = rowAppend(row2, cap2, at2, " run");
		} else {
			at2 = rowAppend(row2, cap2, at2, " fr=");
			at2 = rowAppendDec(row2, cap2, at2, gFailLen);
			/* sei = the SE's M_IRQ_STATUS snapshot, not the gpio39 level: the enable block
			 * below owns "g=", and two different quantities under one key is how a row starts
			 * lying. */
			at2 = rowAppend(row2, cap2, at2, " sei=");
			at2 = rowAppendHex8(row2, cap2, at2, gSnapIrq);
			at2 = rowAppend(row2, cap2, at2, " tx=");
			at2 = rowAppendHex8(row2, cap2, at2, gSnapTxfs);
			at2 = rowAppend(row2, cap2, at2, " rx=");
			at2 = rowAppendHex8(row2, cap2, at2, gSnapRxfs);
			at2 = rowAppend(row2, cap2, at2, " cs=");
			if (gReleaseOk == 0xFFFFFFFFu) {
				at2 = rowAppend(row2, cap2, at2, "-");
			} else {
				at2 = rowAppendDec(row2, cap2, at2, gReleaseOk);
			}
		}
		/* Chip-enable round (dby-touch=6+), printed on EVERY branch because a failed frame read is
		 * exactly when these numbers are the only evidence.  Budget is 24 bytes so id=/row= keep
		 * their place in a 119-byte row:
		 *   en=<rc>         0 = every step answered, 1..8 = the step that stopped (8 = the counter
		 *                    probe failed after the host commands)
		 *   g=<level>/<edges>  gpio39 pad level and high->low count over the 200 ms window
		 *   f=<4 hex bytes>  the raw bytes read at event_buf+0x3F8, unparsed on purpose: the
		 *                    offset is a hypothesis, and printing the bytes themselves is what
		 *                    makes a wrong offset visible instead of an innocent-looking 0. */
		if (gEnableRan) {
			at2 = rowAppend(row2, cap2, at2, " en=");
			if (gEnableRc == 0xFFFFFFFFu) {
				at2 = rowAppend(row2, cap2, at2, "-");
			} else {
				at2 = rowAppendDec(row2, cap2, at2, gEnableRc);
			}
			at2 = rowAppend(row2, cap2, at2, " g=");
			if (gIrqLevel == 0xFFFFFFFFu) {
				at2 = rowAppend(row2, cap2, at2, "-");
			} else {
				at2 = rowAppendDec(row2, cap2, at2, gIrqLevel);
				at2 = rowAppend(row2, cap2, at2, "/");
				at2 = rowAppendDec(row2, cap2, at2, gIrqEdges);
			}
			/* f= is the pre/post counter bytes.  Once the upload runs (stage 8+) its result is
			 * the decisive evidence and the row has no room for both: the frame-failure shape
			 * reaches 119 bytes without up=, so f= (11 bytes) is what gives way.  The counter
			 * itself is still read and its movement is what the '*' used to show. */
			if (gUploadRc == 0xFFFFFFFFu) {
				at2 = rowAppend(row2, cap2, at2, " f=");
				if (gCounterBytes[0] == 0xFFu && gCounterBytes[1] == 0xFFu &&
				    gCounterBytes[2] == 0xFFu && gCounterBytes[3] == 0xFFu) {
					at2 = rowAppend(row2, cap2, at2, "-");
				} else {
					for (unsigned i = 0; i < 4; i++) {
						at2 = rowAppendHex2(row2, cap2, at2, gCounterBytes[i]);
					}
				}
			}
			/* fb= gives way once the upload runs: up=/c= are that round's evidence and the row
			 * has no room for all three (worst failure shape was 121 of 119). */
			if (gUploadRc == 0xFFFFFFFFu) {
			at2 = rowAppend(row2, cap2, at2, " fb=");
			if (gFwState == 0xFFFFFFFFu) {
				at2 = rowAppend(row2, cap2, at2, "-");
			} else {
				at2 = rowAppendHex2(row2, cap2, at2, gFwState);
				/* '+' when the byte lands in the vendor HAL's accepted range. */
				if (gFwState >= DBY_TOUCH_FW_STATE_OK_LO && gFwState <= DBY_TOUCH_FW_STATE_OK_HI) {
					at2 = rowAppend(row2, cap2, at2, "+");
				}
			}
			}
			/* Firmware upload result (stage 8+): 0 = the chip reported a booted firmware,
			 * 1..29 = the step that stopped (see dbyTouchFwAttemptOnce), - = not attempted. */
			if (gUploadRc != 0xFFFFFFFFu) {
				at2 = rowAppend(row2, cap2, at2, " up=");
				at2 = rowAppendDec(row2, cap2, at2, gUploadRc);
				at2 = rowAppend(row2, cap2, at2, "/");
				at2 = rowAppendDec(row2, cap2, at2, gUploadAttempts);
				/* c=<crc flags><mismatches>, read back only after a failed boot: 0x3F133 & 7
				 * says which of ILM/DLM/CRC the ROM accepted, and the mismatch count says
				 * whether the SRAM content matched the golden CRCs. */
				if (gUploadRc != 0u && (gCrcFlags != 0xFFFFFFFFu || gCrcMismatch != 0xFFFFFFFFu)) {
					at2 = rowAppend(row2, cap2, at2, " c=");
					if (gCrcFlags == 0xFFFFFFFFu) {
						at2 = rowAppend(row2, cap2, at2, "-");
					} else {
						at2 = rowAppendDec(row2, cap2, at2, gCrcFlags);
					}
					if (gCrcMismatch == 0xFFFFFFFFu) {
						at2 = rowAppend(row2, cap2, at2, "-");
					} else {
						at2 = rowAppendDec(row2, cap2, at2, gCrcMismatch);
					}
				}
			}
			/* '*' = the bytes changed across the host-command round, i.e. the chip did something
			 * in response to it.  One byte rather than a second full field, because the row has
			 * only a couple of bytes of slack at worst case. */
			if (gCounterMoved) {
				at2 = rowAppend(row2, cap2, at2, "*");
			}
			/* Host-command handshake (stage 7+): 0 = the stamp at 0x2FE77 came back holding the value
			 * we wrote (the N9Z acknowledgement), 1 = it never did, 2 = the transfer failed,
			 * - = not attempted.  Order: bt=<0xC2 path proof> idle=<0x19> active=<0x01>. */
			if (gHostCmdRc[0] != 0xFFu || gHostCmdRc[1] != 0xFFu || gHostCmdRc[2] != 0xFFu) {
				at2 = rowAppend(row2, cap2, at2, " hc=");
				for (unsigned i = 0; i < 3; i++) {
					if (gHostCmdRc[i] == 0xFFu) {
						at2 = rowAppend(row2, cap2, at2, "-");
					} else {
						at2 = rowAppendDec(row2, cap2, at2, gHostCmdRc[i]);
					}
				}
			}
		}
		at2 = rowAppend(row2, cap2, at2, " id=");
		at2 = rowAppendHex8(row2, cap2, at2, gIdentRaw[1]);
		at2 = rowAppend(row2, cap2, at2, " row=");
		if (gIdentRow < 0) {
			at2 = rowAppend(row2, cap2, at2, "-1");
		} else {
			at2 = rowAppendDec(row2, cap2, at2, (unsigned)gIdentRow);
		}
		(void)at2;
	}

	/* Row 2 keeps the stage-1 pin census. It is outside the drawn 0..47 grid, so it is only a
	 * record for the source, not something to look for on screen. */
	char *row3 = native_touch_rows[2];
	const unsigned cap3 = sizeof(native_touch_rows[2]);
	unsigned at3 = 0;
	at3 = rowAppend(row3, cap3, at3, "PINS n=");
	at3 = rowAppendDec(row3, cap3, at3, gPinCfgCount);
	for (unsigned i = 0; i < gPinCfgCount && i < 6; i++) {
		char label[16];
		unsigned n = gPinGpio[i];
		unsigned g0 = n / 100U, g1 = (n / 10U) % 10U, g2 = n % 10U;
		label[0] = ' '; label[1] = 'g';
		label[2] = static_cast<char>('0' + g0);
		label[3] = static_cast<char>('0' + g1);
		label[4] = static_cast<char>('0' + g2);
		label[5] = '='; label[6] = '\0';
		at3 = rowAppend(row3, cap3, at3, label);
		at3 = rowAppendHex8(row3, cap3, at3, gPinCfg[i]);
	}
	(void)at3;
}

/* ------------------------------------------------------------------ */
/* Census                                                             */
/* ------------------------------------------------------------------ */

void
logCensusPin(unsigned pin, const char *role)
{
	DbyTouchMap *tile = tlmmTile(pin);
	uint64_t block;
	uint32_t cfg, in, intrCfg, intrSts, mux;
	char level[4];

	if (tile == nullptr || tile->base == 0) {
		IOLog("DbyTouch: pin%u (%s) not mapped (no tile for it)\n", pin, role);
		return;
	}
	if (!tlmmBlock(pin, &block)) {
		IOLog("DbyTouch: pin%u (%s) outside the mapped tile (%llu bytes)\n",
		    pin, role, (unsigned long long)tile->length);
		return;
	}
	cfg     = rd32(tile->base, block + DBY_TOUCH_TLMM_GPIO_CFG);
	in      = rd32(tile->base, block + DBY_TOUCH_TLMM_GPIO_IN);
	intrCfg = rd32(tile->base, block + DBY_TOUCH_TLMM_INTR_CFG);
	intrSts = rd32(tile->base, block + DBY_TOUCH_TLMM_INTR_STS);
	mux     = (cfg & DBY_TOUCH_CFG_MUX_MASK) >> 2;

	/* IN bit0 is the pad level only while mux == 0.  Pin 43 is muxed to qup14
	 * on this board (BASE:15504), so printing its level as a chip-select level
	 * would be a false reading; say "n/a" instead of a number. */
	if (mux == 0) {
		level[0] = static_cast<char>('0' + (in & 1U));
		level[1] = '\0';
	} else {
		level[0] = 'n';
		level[1] = '/';
		level[2] = 'a';
		level[3] = '\0';
	}

	IOLog("DbyTouch: pin%u %-6s cfg=%08x in=%08x intr_cfg=%08x intr_sts=%08x "
	      "[mux=%u pull=%u drv=%u oe=%u level=%s]\n",
	    pin, role, cfg, in, intrCfg, intrSts, mux,
	    cfg & DBY_TOUCH_CFG_PULL_MASK,
	    (cfg & DBY_TOUCH_CFG_DRV_MASK) >> 6,
	    (cfg & DBY_TOUCH_CFG_OE) != 0 ? 1U : 0U,
	    level);
	if (gPinCfgCount < 6) {
		gPinGpio[gPinCfgCount] = pin;
		gPinCfg[gPinCfgCount++] = cfg;
	}
}

/* Read-only serial-engine identity.  Offsets and decode are the official
 * QcomGeniSpi component's (DbyTouchBoard.h §1b cites the lines).  These reads
 * only mean something once the SE clocks run (GeniSpi.h:211-212), so the
 * verdict distinguishes "engine said X" from "engine said nothing at all". */
void
seCensus(void)
{
	uint32_t fw, status, hw0, dma, ifd, proto, qup, depthMask;
	unsigned major, minor, depth, width;

	/* The wrapper's own identity register, read here rather than in the caller: it used to be
	 * assigned only inside a dead branch, so the page printed qp=00000000 whether or not that word
	 * had ever been read.  The wrapper is a separate window from the engine, so this does not depend
	 * on the engine answering. */
	if (gState.qup.base != 0) {
		gState.qupVersion = rd32(gState.qup.base, DBY_TOUCH_QUPV3_HW_VER_REG);
	}

	fw     = rd32(gState.se.base, DBY_TOUCH_SE_GENI_FW_REVISION_RO);
	status = rd32(gState.se.base, DBY_TOUCH_SE_GENI_STATUS);
	hw0    = rd32(gState.se.base, DBY_TOUCH_SE_HW_PARAM_0);
	dma    = rd32(gState.se.base, DBY_TOUCH_SE_GENI_DMA_MODE_EN);
	ifd    = rd32(gState.se.base, DBY_TOUCH_SE_GENI_IF_DISABLE_RO);
	proto  = (fw >> DBY_TOUCH_SE_FW_PROTO_SHIFT) & 0xFFU;

	gState.seStatus    = status;
	gState.seFwRev     = fw;
	gState.seHwParam0  = hw0;
	gState.seIfDisable = ifd;

	IOLog("DbyTouch: se status=%08x fw_rev=%08x proto=%u hw_param0=%08x "
	      "dma_mode_en=%08x if_disable=%u\n",
	    status, fw, proto, hw0, dma, ifd & DBY_TOUCH_SE_FIFO_IF_DISABLE);

	if (fw == DBY_TOUCH_SE_READ_ALL_ONES || fw == DBY_TOUCH_SE_READ_ALL_ZERO ||
	    status == DBY_TOUCH_SE_READ_ALL_ONES) {
		gState.seVerdict = kSeVerdictUnclocked;
	} else if (proto != DBY_TOUCH_SE_PROTO_SPI) {
		gState.seVerdict = kSeVerdictNotSpi;
	} else if ((ifd & DBY_TOUCH_SE_FIFO_IF_DISABLE) != 0) {
		gState.seVerdict = kSeVerdictFifoDisabled;
	} else {
		gState.seVerdict = kSeVerdictFifoUsable;
	}

	IOLog("DbyTouch: se verdict=%s\n", seVerdictText(gState.seVerdict));
	if (gState.seVerdict == kSeVerdictUnclocked) {
		IOLog("DbyTouch: se identity looks gated, not broken: the QUP branch is "
		      "not clocked.  This driver performs no GCC vote (README §12), so "
		      "stages 3..5 cannot run on this boot.\n");
	}
	if (gState.seVerdict == kSeVerdictFifoUsable) {
		qup   = gState.qupVersion;
		major = (unsigned)((qup >> DBY_TOUCH_QUPV3_HW_MAJOR_SHIFT) & 0xFU);
		minor = (unsigned)((qup >> DBY_TOUCH_QUPV3_HW_MINOR_SHIFT) &
		                   DBY_TOUCH_QUPV3_HW_MINOR_MASK);
		depthMask = ((major == DBY_TOUCH_QUPV3_NEW_FIFO_MAJOR &&
		              minor >= DBY_TOUCH_QUPV3_NEW_FIFO_MINOR) ||
		             major > DBY_TOUCH_QUPV3_NEW_FIFO_MAJOR)
		                ? DBY_TOUCH_SE_FIFO_DEPTH_MASK_NEW
		                : DBY_TOUCH_SE_FIFO_DEPTH_MASK_OLD;
		depth = (unsigned)((hw0 & depthMask) >> DBY_TOUCH_SE_FIFO_DEPTH_SHIFT);
		width = (unsigned)((hw0 >> DBY_TOUCH_SE_FIFO_WIDTH_SHIFT) &
		                   DBY_TOUCH_SE_FIFO_WIDTH_MASK);
		IOLog("DbyTouch: se fifo depth=%u words width=%u bits proto=%u "
		      "(decode: GeniSpi.c:30-33)\n", depth, width, proto);
	}
}

/* Stage 1: nothing is written.  This is the census that tells us whether the
 * firmware/ABL left the touch pads in a usable state, whether the reset line is
 * already released, and whether the serial engine is even clocked before we
 * ever ask for power. */
void
census(void)
{
	if (gState.censusLogged) {
		return;
	}
	gState.censusLogged = true;

	IOLog("DbyTouch: stage=%u ic=%s transport=%s compatible=%s\n",
	    gState.stage, DBY_TOUCH_IC_FAMILY, DBY_TOUCH_IC_TRANSPORT, DBY_TOUCH_COMPATIBLE);
	/* Build identity: the same string is greppable in the linked image, which is
	 * how "the image really contains this round's code" is checked before a
	 * flash (README §14 step 0). */
	IOLog("DbyTouch: source rev %s\n", DBY_TOUCH_SOURCE_REV);
	IOLog("DbyTouch: tlmm=0x%llx se=0x%llx qup=0x%llx\n",
	    DBY_TOUCH_TLMM_BASE, DBY_TOUCH_SE_BASE, DBY_TOUCH_QUP_WRAPPER_BASE);

	logCensusPin(DBY_TOUCH_GPIO_IRQ, "irq");
	logCensusPin(DBY_TOUCH_GPIO_RESET, "reset");
	logCensusPin(DBY_TOUCH_GPIO_CS, "cs");
	logCensusPin(DBY_TOUCH_GPIO_SPI_0, "spi40");
	logCensusPin(DBY_TOUCH_GPIO_SPI_1, "spi41");
	logCensusPin(DBY_TOUCH_GPIO_SPI_2, "spi42");

	/* Stage 1 touches the always-on TLMM block only.
	 *
	 * 2026-10-31 hardware result: with dby-touch=1 the device stopped on the diagnostic page and
	 * never reached the lock screen. The census then read GENI_STATUS / HW_PARAM_0 on an SE whose
	 * QUP branch nobody has clocked yet, and an unclocked read on this interconnect can stall the
	 * bus instead of faulting. The pads live in the always-on TLMM, so the pin census is safe on
	 * its own; the QUP wrapper and SE identity need stage 2 (dby-touch=2), which is only correct
	 * once the branch clock is voted.
	 */
	if (gState.qup.base != 0 || gState.se.base != 0) {
		/* 审核 45584a61 B2：stage 号不是安全门。这个驱动从不做 GCC 时钟投票，SE 未上电时读它会
		 * 把总线挂死（2026-10-31 已实测）。所以 SE/QUP 的读取必须额外显式打开：
		 *   dby-touch=<n> dby-touch-se=1
		 * 两个条件都满足才读；否则只读常开域 TLMM，并在行里把原因写出来。 */
		int want_se = 0;
		PE_parse_boot_argn("dby-touch-se", &want_se, sizeof want_se);
		static bool logged_gate = false;
		if (false && want_se) {   /* never from here: see the note below */
			if (gState.qup.base != 0) {
				gState.qupVersion = rd32(gState.qup.base, DBY_TOUCH_QUPV3_HW_VER_REG);
				IOLog("DbyTouch: qup wrapper hw version @+0x%x = %08x\n",
				    DBY_TOUCH_QUPV3_HW_VER_REG, gState.qupVersion);
			}
			if (gState.se.base != 0) {
				seCensus();
			}
		} else if (!logged_gate) {
			logged_gate = true;
			/* This runs from boot_progress_poll() at whatever moment the poller happens to call,
			 * and on 2026-10-31 a serial-engine read from here stalled the interconnect because the
			 * QUP clocks had not been voted yet.  Stage 2 votes them on its own thread and reads the
			 * engine only after the branches report running, so the census never touches it. */
			IOLog("DbyTouch: qup/se reads skipped here (stage=%u, dby-touch-se=%d); stage 2 does them "
			      "after the clock vote\n", gState.stage, want_se);
		}
	}
	IOLog("DbyTouch: stage %u census done (no register written)\n", gState.stage);
	publishPageRow();
}

/* Stage 2: the first stage allowed to write.  It only touches the two pins the
 * overlay's own pinctrl nodes describe, one field group at a time, and
 * read-back verifies every field it claimed to set — including the output
 * enable bit, which the first version left out of the comparison mask. */
void
applyPinConfig(void)
{
	struct {
		unsigned pin;
		uint32_t pull;
		uint32_t drvShifted;
		const char *role;
	} plan[] = {
		/* DTBO:9991-10007  pmx_ts_reset_active: gpio98, function gpio, 8 mA,
		 * bias-disable.  OE stays 0: the reset sequence that needs the pad
		 * driven is stage 4, and this stage must not move the IC's reset. */
		{ DBY_TOUCH_GPIO_RESET, DBY_TOUCH_CFG_PULL_DISABLE, 2U << 6, "reset" },
		/* DTBO:9955-9970  pmx_ts_int_active: gpio39, function gpio, 8 mA,
		 * bias-pull-up, input (oe = 0). */
		{ DBY_TOUCH_GPIO_IRQ,   DBY_TOUCH_CFG_PULL_UP,      2U << 6, "irq" },
	};
	const uint32_t verifyMask = DBY_TOUCH_CFG_MUX_MASK | DBY_TOUCH_CFG_PULL_MASK |
	                            DBY_TOUCH_CFG_DRV_MASK | DBY_TOUCH_CFG_OE;
	unsigned i;

	if (gState.tlmmSouth.base == 0 && gState.tlmmNorth.base == 0) {
		IOLog("DbyTouch: pin config skipped: no pin tile mapped\n");
		return;
	}

	for (i = 0; i < sizeof(plan) / sizeof(plan[0]); i++) {
		uint64_t block;
		uint32_t was, want;

		DbyTouchMap *tile = tlmmTile(plan[i].pin);

		if (tile == nullptr || !tlmmBlock(plan[i].pin, &block)) {
			IOLog("DbyTouch: pin%u (%s) outside the mapped TLMM window "
			      "(%llu bytes), not written\n",
			    plan[i].pin, plan[i].role,
			    (unsigned long long)gState.tlmm.length);
			gState.pinWriteFailures++;
			continue;
		}
		was  = rd32(tile->base, block + DBY_TOUCH_TLMM_GPIO_CFG);
		want = was;
		want &= ~(DBY_TOUCH_CFG_MUX_MASK | DBY_TOUCH_CFG_PULL_MASK |
		          DBY_TOUCH_CFG_DRV_MASK);
		want |= plan[i].pull;          /* mux stays 0 = GPIO */
		want |= plan[i].drvShifted;
		want &= ~DBY_TOUCH_CFG_OE;     /* both pins stay inputs at this stage */

		if (want == was) {
			IOLog("DbyTouch: pin%u (%s) cfg already %08x\n", plan[i].pin, plan[i].role, was);
			continue;
		}
		gState.pinWrites++;
		if (wr32Checked(tile->base, block + DBY_TOUCH_TLMM_GPIO_CFG, want,
		        verifyMask)) {
			/* Publish what the pin reads *now*.  The page used to show the census snapshot taken
			 * before this write, so a correctly configured pin looked exactly like an untouched
			 * one and "did the write land?" was unanswerable without a UART. */
			pinCfgRefresh(plan[i].pin, rd32(tile->base, block + DBY_TOUCH_TLMM_GPIO_CFG));
			IOLog("DbyTouch: pin%u (%s) cfg %08x -> %08x\n",
			    plan[i].pin, plan[i].role, was,
			    rd32(tile->base, block + DBY_TOUCH_TLMM_GPIO_CFG));
		} else {
			gState.pinWriteFailures++;
			IOLog("DbyTouch: pin%u (%s) cfg %08x -> %08x NOT RETAINED (want %08x)\n",
			    plan[i].pin, plan[i].role, was,
			    rd32(tile->base, block + DBY_TOUCH_TLMM_GPIO_CFG), want);
		}
	}
	publishPageRow();
}

bool
mapWindows(void)
{
	/* TLMM: one mapping per tile.  A pin's registers are at tile_base + pin * stride, and the
	 * touch pins span two tiles (south for 39/40/41/42/43, north for 98), so mapping a single
	 * window and walking pin * stride from it reads unused slots instead of the pins.  That is
	 * exactly what produced six 0x00000000 GPIO_CFG words on 2026-10-31.
	 * Length: up to and including the highest pin in that tile; a pin the tile does not cover is
	 * rejected by tlmmBlock(). */
	const uint64_t southBytes = (static_cast<uint64_t>(DBY_TOUCH_GPIO_CS) + 1ULL) *
	                            DBY_TOUCH_TLMM_STRIDE;
	/* Pin ids are global, not tile-local: the north tile's pin 98 lives at 98 * stride, so the
	 * window has to reach that far (a one-page mapping reports "outside the tile" and the reset pin
	 * is never configured). */
	const uint64_t northBytes = (static_cast<uint64_t>(DBY_TOUCH_GPIO_RESET) + 1ULL) *
	                            DBY_TOUCH_TLMM_STRIDE;
	const uint64_t westBytes = DBY_TOUCH_TLMM_STRIDE;

	if (gState.tlmmSouth.base == 0 &&
	    !mapPhys(DBY_TOUCH_TLMM_BASE_SOUTH, southBytes, &gState.tlmmSouth)) {
		IOLog("DbyTouch: cannot map TLMM south at 0x%llx\n", DBY_TOUCH_TLMM_BASE_SOUTH);
		return false;
	}
	/* The north tile is addressed the same way as the others, but its pin numbering restarts, so
	 * only the pages this driver touches are mapped.  The north tile starts at 0x0F900000. */
	if (gState.tlmmNorth.base == 0 &&
	    !mapPhys(DBY_TOUCH_TLMM_BASE_NORTH, northBytes, &gState.tlmmNorth)) {
		IOLog("DbyTouch: cannot map TLMM north at 0x%llx\n", DBY_TOUCH_TLMM_BASE_NORTH);
		return false;
	}
	/* West stays mapped for the probe read and for the sources that still name it. */
	if (gState.tlmm.base == 0 &&
	    !mapPhys(DBY_TOUCH_TLMM_BASE_WEST, westBytes, &gState.tlmm)) {
		IOLog("DbyTouch: cannot map TLMM west at 0x%llx\n", DBY_TOUCH_TLMM_BASE_WEST);
		return false;
	}
	if (gState.se.base == 0 &&
	    !mapPhys(DBY_TOUCH_SE_BASE, DBY_TOUCH_SE_SIZE, &gState.se)) {
		IOLog("DbyTouch: cannot map GENI SE at 0x%llx\n", DBY_TOUCH_SE_BASE);
		return false;
	}
	if (gState.qup.base == 0 &&
	    !mapPhys(DBY_TOUCH_QUP_WRAPPER_BASE, DBY_TOUCH_QUP_WRAPPER_SIZE, &gState.qup)) {
		IOLog("DbyTouch: cannot map QUP wrapper at 0x%llx\n", DBY_TOUCH_QUP_WRAPPER_BASE);
		return false;
	}
	return true;
}

void
unmapWindows(void)
{
	unmapWindow(&gState.se);
	unmapWindow(&gState.qup);
	unmapWindow(&gState.tlmm);
	unmapWindow(&gState.tlmmSouth);
	unmapWindow(&gState.tlmmNorth);
}

uint32_t
parseStage(void)
{
	int value = DBY_TOUCH_STAGE_OFF;

	if (PE_parse_boot_argn(DBY_TOUCH_ARG_KEY, &value, sizeof(value))) {
		if (value < 0) {
			value = DBY_TOUCH_STAGE_OFF;
		}
		if (value > DBY_TOUCH_STAGE_FWUPLOAD) {
			value = DBY_TOUCH_STAGE_FWUPLOAD;
		}
	} else {
		value = DBY_TOUCH_STAGE_OFF;
	}
	/* The page's "touch mode" flag is non-zero exactly when a stage was asked
	 * for, so the requested stage is the natural value to publish.  Zero (no
	 * boot argument) leaves row 42 with the TIMER line it shows today. */
	native_touch_stage_req = (unsigned)value;
	return (uint32_t)value;
}

} /* namespace */

/* ------------------------------------------------------------------ */
/* IOService                                                          */
/* ------------------------------------------------------------------ */

class DbyTouch : public IOService {
	OSDeclareDefaultStructors(DbyTouch)

public:
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
};

OSDefineMetaClassAndStructors(DbyTouch, IOService)

bool
DbyTouch::start(IOService *provider)
{
	if (provider == nullptr) {
		IOLog("DbyTouch: started without a provider\n");
		return false;
	}
	if (!IOService::start(provider)) {
		return false;
	}

	gState.stage = parseStage();

	/* The DT node carries reg/interrupts/pinctrl; log whether we actually got a
	 * provider with a reg property, because "started with fallback constants"
	 * and "started from device tree" must never be confused in a log. */
	OSData *reg = OSDynamicCast(OSData, provider->getProperty("reg"));
	if (reg != nullptr) {
		IOLog("DbyTouch: provider reg property present (%u bytes)\n", reg->getLength());
	} else {
		IOLog("DbyTouch: provider has no reg property; using DbyTouchBoard.h constants\n");
	}

	if (!mapWindows()) {
		IOLog("DbyTouch: window mapping failed, stopping\n");
		/* A false return means IOKit never calls stop(), so nothing that was
		 * already mapped here would ever be released: undo both halves. */
		unmapWindows();
		IOService::stop(provider);
		return false;
	}

	census();

	if (gState.stage >= DBY_TOUCH_STAGE_CLOCKS) {
		applyPinConfig();
	} else {
		IOLog("DbyTouch: stage %u < %u, no register written\n", gState.stage,
		    (unsigned)DBY_TOUCH_STAGE_CLOCKS);
	}

	IOLog("DbyTouch: started (stages 3..5 need the IC protocol and a clocked "
	      "serial engine; boot-arg %s=<n> selects a stage)\n", DBY_TOUCH_ARG_KEY);
	registerService();
	return true;
}

void
DbyTouch::stop(IOService *provider)
{
	unmapWindows();
	gState.censusLogged = false;
	gState.stage = DBY_TOUCH_STAGE_OFF;
	gState.seVerdict = kSeVerdictUnknown;
	IOLog("DbyTouch: stopped\n");
	IOService::stop(provider);
}

/* ------------------------------------------------------------------ */
/* Provider-less entry point                                          */
/* ------------------------------------------------------------------ */

/*
 * Early bring-up hook.  A personality in iokit/KernelConfigTables.cpp is what
 * normally instantiates a driver, and that file lives under patches/ which this
 * work must not touch.  Until the personality lands, an existing early-stage
 * prober can call this symbol directly: it maps the windows and runs the same
 * census without attaching to a provider.
 *
 * Returns 0 on success (windows mapped and census printed).
 */


/* Bytes that carry no information in the report area.  0x1F is measured fill: sixteen consecutive
 * 0x1F bytes sit at frame offset 1276, which an "is it 0x00 or 0xFF" test reported as the frame's
 * longest data run.  Treating it as idle is what makes the scan point at real content. */
static inline bool
dbyFrameIdle(uint8_t b)
{
	return b == 0x00 || b == 0xFF || b == 0x1F;
}

/* The frame is 8 KiB and every open question about its layout is easier to answer on a host than
 * through one hex field per flash.  With dby-touch-dump=1 the whole frame goes to the export region
 * that sits below the diagnostic log window, so it can be read back over EDL and parsed offline.
 *
 * Three things this has to respect, each learned from the first attempt:
 *   - the write API takes an offset inside the region, not an absolute LBA;
 *   - it copies whole 4 KiB blocks, so an 8208-byte frame needs a zero-padded three-block buffer;
 *   - the log window belongs to the log writer, and the export region only exists once that window
 *     is open, so the dump waits for it rather than opening anything of its own.
 */
extern "C" int native_ufs_export_write(uint64_t, uint32_t, const void *) __attribute__((weak_import));
extern "C" uint64_t native_ufs_export_blocks(void) __attribute__((weak_import));

/* Dump frame `index` into the export region.  Slot layout, from the end of the region backwards:
 *   index 0 -> blocks total-3 .. total-1
 *   index 1 -> blocks total-6 .. total-4
 *   index 2 -> blocks total-9 .. total-7
 *   index 3 -> blocks total-12 .. total-10
 * Four frames in ONE boot is what makes a touch measurement possible: the chip's adaptive baseline
 * settles differently on every boot (two idle frames from consecutive boots correlate only 0.295),
 * so comparing frames across boots cannot separate a finger from that drift.  Comparing frames
 * inside one boot can. */
static void
dbyDumpFrame(const uint8_t *frame, size_t len, unsigned index)
{
	const uint32_t kBlocks = 3;                      /* 12288 bytes: covers the 8208-byte frame */
	uint8_t *staging;
	uint64_t total;
	unsigned attempt;

	if (native_ufs_export_write == NULL || native_ufs_export_blocks == NULL) {
		gDumpOk = 2;                                 /* the transport is not in this kernel */
		return;
	}
	/* The export region appears only after the log writer has opened the window; stage 5 runs a few
	 * seconds into boot, so this waits briefly instead of giving up on the first look. */
	for (attempt = 0; attempt < 40; attempt++) {
		total = native_ufs_export_blocks();
		if (total >= kBlocks + 16) {
			break;
		}
		IOSleep(100);
	}
	if (total < kBlocks + 16) {
		gDumpOk = 3;                                 /* the log window never opened */
		return;
	}

	staging = (uint8_t *)IOMallocAligned(kBlocks * 4096, 4096);
	if (staging == nullptr) {
		gDumpOk = 4;
		return;
	}
	for (size_t i = 0; i < kBlocks * 4096; i++) {
		staging[i] = 0;
	}
	for (size_t i = 0; i < len && i < kBlocks * 4096; i++) {
		staging[i] = frame[i];
	}
	/* Near the end of the region, far from anything else that writes there, one 3-block slot per
	 * frame going backwards. */
	if (total < (uint64_t)kBlocks * (DBY_TOUCH_FRAME_SNAPSHOTS + 1)) {
		IOFreeAligned(staging, kBlocks * 4096);
		gDumpOk = 3;
		return;
	}
	gDumpRel = (uint32_t)(total - (uint64_t)kBlocks * (index + 1));
	gDumpOk = native_ufs_export_write(gDumpRel, kBlocks, staging) ? 1u : 5u;
	IOFreeAligned(staging, kBlocks * 4096);
}

/* ------------------------------------------------------------------ */
/* Stage 3: GENI/SPI                                                    */
/* ------------------------------------------------------------------ */

/* The register-access callbacks the official driver expects.  ctx is the DbyTouchMap of the window
 * the driver is talking to (engine for geni_spi->se, wrapper for ->wrapper). */
static uint32_t
dbyGeniRd(void *ctx, uint32_t reg)
{
	DbyTouchMap *m = (DbyTouchMap *)ctx;

	return m != nullptr ? rd32(m->base, reg) : 0xFFFFFFFFu;
}

static void
dbyGeniWr(void *ctx, uint32_t reg, uint32_t value)
{
	DbyTouchMap *m = (DbyTouchMap *)ctx;

	if (m != nullptr) {
		wr32Checked(m->base, reg, value, value);
	}
}

static void
dbyGeniDelay(void *ctx, unsigned us)
{
	(void)ctx;
	IODelay(us);
}

/* Both workers are started from the previous stage's thread, so they are forward-declared. */
static void stage3Thread(void *arg, wait_result_t wait);
static void stage4Thread(void *arg, wait_result_t wait);
static void stage5Thread(void *arg, wait_result_t wait);

/* Runs the ladder's first three steps and publishes what it saw.  Every step stops on failure:
 * an unclocked engine or a wrong mode must not be followed by a transfer. */
static void
stage3Thread(void *arg, wait_result_t wait)
{
	int rc;
	uint8_t tx[8], rx[8];
	unsigned i;

	(void)arg; (void)wait;
	thread_set_thread_name(current_thread(), "dby_touch_s3");

	gSpi.se.ctx = &gState.se;
	gSpi.se.rd = dbyGeniRd;
	gSpi.se.wr = dbyGeniWr;
	gSpi.se.delay_us = dbyGeniDelay;
	gSpi.wrapper.ctx = &gState.qup;
	gSpi.wrapper.rd = dbyGeniRd;
	gSpi.wrapper.wr = dbyGeniWr;
	gSpi.wrapper.delay_us = dbyGeniDelay;
	/* 2: leave the engine's interrupt outputs enabled.
	 *
	 * HYPOTHESIS, not a result: the FIFO path feeds TX in 60-byte batches and sets the watermark
	 * between them (GeniSpi.c:156-174), so a transfer longer than one batch needs the engine to make
	 * progress on its own.  The first long attempts failed (64 and 8208 bytes) while 16 bytes passed,
	 * which fits "the watermark path needs the interrupt enables", but it does not prove it: the
	 * 16-byte run succeeded with poll_only=1, and its completion bit comes from the same
	 * M_IRQ_STATUS the watermark path polls, so that status keeps updating either way.  This run is
	 * the experiment: same length change, interrupt enables restored.
	 *
	 * No handler is attached to this SE's GIC line and XNU disables every SPI in the distributor at
	 * startup, so the raised bits stay pending there instead of reaching a CPU. */
	gSpi.poll_only = 2;
	gSpi.poll_limit = 200000;

	/* L1: identity.  Read only, so a failure here costs nothing. */
	rc = geni_spi_read_info(&gSpi);
	gStage3Info = ((gSpi.info.protocol & 0xFFu) << 24) |
	              ((gSpi.info.fifo_width_bits & 0xFFu) << 16) |
	              (gSpi.info.tx_fifo_depth & 0xFFFFu);
	if (rc != 0 || gSpi.info.protocol != GENI_PROTO_SPI ||
	    gSpi.info.fifo_width_bits != 32 || gSpi.info.tx_fifo_depth < 4) {
		gStage3Rc = 1;
		publishPageRow();
		return;
	}

	/* L2: init, clock, mode 0.  sclk is the SE root's own rate: the RCG was programmed to CXO with
	 * DIV=1 earlier, so 19.2 MHz is the value this driver asked for. */
	rc = geni_spi_init(&gSpi);
	if (rc != 0) {
		gStage3Rc = 2;
		publishPageRow();
		return;
	}
	/* The vendor runs this link at 16 MHz (its root clock is faster); ours is CXO 19.2 MHz, so the
	 * fastest whole division is 2, i.e. 9.6 MHz.  The frame is 8 KiB, and at 1 MHz one frame takes
	 * 66 ms, which is several report periods - the answer would always be a stale frame. */
	rc = geni_spi_set_clock(&gSpi, 9600000u, 19200000u, 0);
	if (rc != 0) {
		gStage3Rc = 3;
		publishPageRow();
		return;
	}
	rc = geni_spi_setup(&gSpi, DBY_TOUCH_SPI_MODE, 0);
	if (rc != 0) {
		gStage3Rc = 4;
		publishPageRow();
		return;
	}

	/* L3: loopback.  The engine's own MOSI/MISO path, so it proves the controller without needing
	 * the pads, the chip or its power - exactly the split the bring-up ladder asks for. */
	(void)geni_spi_setup(&gSpi, GENI_SPI_LOOP, 0);
	tx[0] = 0xA5; tx[1] = 0x5A; tx[2] = 0xC3; tx[3] = 0x3C;
	for (i = 0; i < 4; i++) {
		rx[i] = 0;
	}
	rc = geni_spi_transfer(&gSpi, tx, rx, 4, 0);
	(void)geni_spi_setup(&gSpi, DBY_TOUCH_SPI_MODE, 0);
	gStage3Loop = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) |
	              ((uint32_t)rx[2] << 8) | (uint32_t)rx[3];
	gStage3Irq = gSpi.snap_irq;
	gStage3Status = gSpi.snap_status;
	if (rc != 0 || gStage3Loop != 0xA55AC33Cu) {
		gStage3Rc = 5;
		publishPageRow();
		return;
	}
	gStage3Rc = 0;
	publishPageRow();
	IOLog("DbyTouch(stage3): loopback ok, info=%08x\n", gStage3Info);

	/* The controller is proven, so the next step may touch the pads and the chip. */
	{
		thread_t stage4;

		if (kernel_thread_start(stage4Thread, NULL, &stage4) == KERN_SUCCESS) {
			thread_deallocate(stage4);
		}
	}
}


/* Mux the four SPI pads to their QUP function.
 *
 * Until now every access went through the engine's own loopback, which never reaches a pad.  Reading
 * the chip is the first thing that needs the pins, and they are not muxed: the census reads
 * 0x00000001 for all of them, i.e. function 0 = GPIO.  The vendor active state is function 1 with
 * drive strength 8 mA and bias disabled, which is what this writes.
 */
static int
applySpiPinMux(void)
{
	static const unsigned pins[4] = {
		DBY_TOUCH_GPIO_SPI_0, DBY_TOUCH_GPIO_SPI_1, DBY_TOUCH_GPIO_SPI_2,
		DBY_TOUCH_GPIO_CS
	};

	for (unsigned i = 0; i < 4; i++) {
		DbyTouchMap *tile = tlmmTile(pins[i]);
		uint64_t block;
		uint32_t was, want;

		if (tile == nullptr || !tlmmBlock(pins[i], &block)) {
			return DBY_TOUCH_ERR_IO;
		}
		was = rd32(tile->base, block + DBY_TOUCH_TLMM_GPIO_CFG);
		if (i == 0) {
			gMuxBefore = was;
		}
		want = was;
		want &= ~(DBY_TOUCH_CFG_MUX_MASK | DBY_TOUCH_CFG_PULL_MASK | DBY_TOUCH_CFG_DRV_MASK);
		want |= (1U << 2);                       /* function 1 = qup14 (funcs[0] is gpio)     */
		/* drive-strength in the vendor DT is <6>, and pinctrl-msm encodes it as (6/2 - 1) = 2. */
		want |= (2U << 6);
		want &= ~DBY_TOUCH_CFG_OE;               /* the engine drives these pads, not the CPU */
		if (!wr32Checked(tile->base, block + DBY_TOUCH_TLMM_GPIO_CFG, want,
		        DBY_TOUCH_CFG_MUX_MASK | DBY_TOUCH_CFG_DRV_MASK)) {
			return DBY_TOUCH_ERR_IO;
		}
		pinCfgRefresh(pins[i], rd32(tile->base, block + DBY_TOUCH_TLMM_GPIO_CFG));
		if (i == 0) {
			gMuxAfter = rd32(tile->base, block + DBY_TOUCH_TLMM_GPIO_CFG);
		}
	}
	return DBY_TOUCH_OK;
}

/* Bridge the protocol layer's bus to the GENI engine. */
static int
dbyBusXfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
	struct geni_spi *spi = (struct geni_spi *)ctx;
	uint8_t scratch[64];
	size_t i;

	if (len > sizeof scratch) {
		return DBY_TOUCH_ERR_PARAM;
	}
	if (rx == NULL) {
		/* The protocol layer passes rx == NULL for write-only transfers; the engine still needs a
		 * buffer to land the shifted-out bytes in. */
		for (i = 0; i < len; i++) {
			scratch[i] = 0;
		}
		rx = scratch;
	}
	return geni_spi_transfer(spi, tx, rx, len, 0) == 0 ? DBY_TOUCH_OK : DBY_TOUCH_ERR_IO;
}

static void
dbyBusDelay(void *ctx, unsigned ms)
{
	(void)ctx;
	IOSleep(ms);
}

static void
stage4Thread(void *arg, wait_result_t wait)
{
	struct dby_touch_bus bus;
	struct dby_touch_identify id;
	int rc;

	(void)arg; (void)wait;
	thread_set_thread_name(current_thread(), "dby_touch_s4");

	rc = applySpiPinMux();
	if (rc != DBY_TOUCH_OK) {
		gStage4Rc = 1;
		publishPageRow();
		return;
	}

	bus.ctx = &gSpi;
	bus.xfer = dbyBusXfer;
	bus.delay_ms = dbyBusDelay;

	for (unsigned i = 0; i < 8; i++) {
		id.raw[i] = 0;
	}
	id.trim_row = -1;
	id.attempts = 0;

	rc = dby_touch_identify_n9z(&bus, &id);
	gIdentRaw[0] = ((uint32_t)id.raw[0] << 24) | ((uint32_t)id.raw[1] << 16) |
	               ((uint32_t)id.raw[2] << 8) | (uint32_t)id.raw[3];
	gIdentRaw[1] = ((uint32_t)id.raw[4] << 24) | ((uint32_t)id.raw[5] << 16) |
	               ((uint32_t)id.raw[6] << 8) | (uint32_t)id.raw[7];
	gIdentRow = id.trim_row;
	gStage4Rc = (rc == DBY_TOUCH_OK) ? 0u : (rc == DBY_TOUCH_ERR_PROTO ? 2u : 3u);
	publishPageRow();

	/* Only a chip that answered its own identity may be asked for a frame. */
	if (rc == DBY_TOUCH_OK) {
		thread_t stage5;

		if (kernel_thread_start(stage5Thread, NULL, &stage5) == KERN_SUCCESS) {
			thread_deallocate(stage5);
		}
	}
	IOLog("DbyTouch(stage4): identify rc=%d row=%d raw=%08x %08x\n", rc, id.trim_row,
	    gIdentRaw[0], gIdentRaw[1]);
}


/* Reads a frame longer than the engine can carry in one command.
 *
 * The length ladder measured the boundary: 16, 60, 64 and 128 bytes all succeed and 512 fails with
 * the RX FIFO sitting at its ready-for-receive watermark (14 of 16 words) while TX is full and the
 * command is still active - the engine stopped because the host did not drain fast enough, not
 * because the chip refused.  Rather than change the upstream component's draining, the frame is
 * taken in 128-byte pieces with CS held between them (GENI_M_PARAM_FRAGMENTATION, which is what
 * keep_cs = 1 sets), so the chip still sees one transaction.
 */
#define DBY_TOUCH_FRAME_CHUNK 128

static int
stage5ReadChunked(uint8_t *buf, size_t len, size_t *done_out)
{
	size_t done = 0;

	*done_out = 0;
	gReleaseOk = 0xFFFFFFFFu;
	while (done < len) {
		size_t chunk = len - done;
		uint8_t tx[DBY_TOUCH_FRAME_CHUNK];
		int keep_cs;

		if (chunk > DBY_TOUCH_FRAME_CHUNK) {
			chunk = DBY_TOUCH_FRAME_CHUNK;
		}
		for (size_t i = 0; i < chunk; i++) {
			tx[i] = 0;
		}
		if (done == 0) {
			tx[0] = 0x20;          /* the report request goes out with the first piece only */
		}
		/* Every piece but the last keeps CS asserted, so the chip sees one transaction.  A failure
		 * on a middle piece therefore leaves CS held low, and nothing in the component releases it. */
		keep_cs = (done + chunk < len) ? 1 : 0;
		if (geni_spi_transfer(&gSpi, tx, buf + done, chunk, keep_cs) != 0) {
			/* Snapshot first: the release transfer below overwrites these registers, and the row has
			 * to show the state of the transfer that actually failed. */
			gSnapIrq = gSpi.snap_irq;
			gSnapStatus = gSpi.snap_status;
			gSnapTxfs = gSpi.snap_txfs;
			gSnapRxfs = gSpi.snap_rxfs;

			/* A failed *middle* piece is the one that strands the chip; a failure on the last piece
			 * already ended the frame.  The release is harmless either way and its own result is
			 * published, because a silently failing release is exactly the "chip stuck until reboot"
			 * case.  Four bytes is a whole number of 32-bit SPI words, which is all the engine needs. */
			if (keep_cs) {
				uint8_t release[4];

				for (size_t i = 0; i < sizeof release; i++) {
					release[i] = 0;
				}
				gReleaseOk = (geni_spi_transfer(&gSpi, release, release, sizeof release, 0) == 0) ? 1u : 0u;
			}
			*done_out = done;
			return DBY_TOUCH_ERR_IO;
		}
		done += chunk;
	}
	*done_out = done;
	return DBY_TOUCH_OK;
}

/* ------------------------------------------------------------------ */
/* Chip enable (stage 6+)                                             */
/* ------------------------------------------------------------------ */

/* Why this exists: the chip answers its identity and returns frames, but a finger held on the
 * panel leaves the frame unchanged (measured 2026-11-02: the structured 0..0xFFF region does not
 * move at all while the 0x1000+ region is stable per boot and random across boots).  The vendor
 * kernel's own bring-up is the missing step, and it is only this:
 *
 *     novatek_thp.c:310-333   rst98 LOW -> write 0xAA to 0x3F0FE -> rst98 HIGH (1 ms)
 *
 * i.e. the chip is reset while the reset line is asserted, then released.  This driver never
 * touched gpio98: identity worked because the firmware leaves the chip powered and out of reset,
 * but a chip that was never released by its driver does not start scanning.
 *
 * Register facts used here, each with its source:
 *   - gpio98 is the reset line (huawei_tp.dtsi rst_gpio <&tlmm 98 0>), HIGH = running.
 *   - the word at 0x3F0FE is the vendor's reset-to-idle byte: 0x69 = into bootloader, 0xAA = idle
 *     (novatek_thp.c:310-333 and this driver's own identify, DbyTouchProtocol.c:138-147).
 *   - gpio39 is the touch interrupt: idle HIGH, falling edge when a frame is ready.  Its trigger is
 *     irq_flag = <2> in the novatech child node (huawei_tp.dtsi:143), not the DT interrupts cell,
 *     which the QCOM gpio_to_irq path ignores.
 *   - TLMM per-pin block = tile_base + pin * 0x1000, CFG at +0, IN/OUT at +4, INTR_CFG at +8.
 *     Field layout (pinctrl-msm.c PINGROUP): pull[1:0], func[5:2], drv[8:6], oe[9]; the function
 *     field is four bits wide, which is why DBY_TOUCH_CFG_MUX_MASK is 0x3C and not 0x1C.
 *   - drv encoding is (n+1)*2 mA, so 8 mA is 0b011 (3), not 0b010.
 *
 * The sequence is deliberately observable: the reset value and the IRQ line are read back and
 * published, and the frame counter is sampled twice, because the interesting answer this round is
 * "did anything start moving", including a negative one. */

/* pinctrl-msm PINGROUP encodings come from DbyTouchBoard.h (DBY_TOUCH_CFG_PULL_*, _MUX_MASK,
 * _DRV_MASK, _DRV_8MA, _OE): one definition per value, next to the citation, so the sequence below
 * cannot drift from the census that reads the same fields back. */

static int
dbyTouchPinWrite(unsigned pin, uint32_t want, uint32_t mask)
{
	DbyTouchMap *tile = tlmmTile(pin);
	uint64_t block;

	if (tile == nullptr || !tlmmBlock(pin, &block)) {
		return DBY_TOUCH_ERR_IO;
	}
	if (!wr32Checked(tile->base, block + DBY_TOUCH_TLMM_GPIO_CFG, want, mask)) {
		return DBY_TOUCH_ERR_IO;
	}
	return DBY_TOUCH_OK;
}

static int
dbyTouchPinRead(unsigned pin, uint32_t *cfg, uint32_t *io)
{
	DbyTouchMap *tile = tlmmTile(pin);
	uint64_t block;

	if (tile == nullptr || !tlmmBlock(pin, &block)) {
		return DBY_TOUCH_ERR_IO;
	}
	if (cfg != nullptr) {
		*cfg = rd32(tile->base, block + DBY_TOUCH_TLMM_GPIO_CFG);
	}
	if (io != nullptr) {
		*io = rd32(tile->base, block + DBY_TOUCH_TLMM_GPIO_IN);
	}
	return DBY_TOUCH_OK;
}

/* Write one pad's level.  The level lives in bit 1 of the per-pin IO register at +4, NOT in
 * GPIO_CFG: pinctrl-msm.c:451-456 does exactly this (read io_reg, set/clear out_bit, write it back)
 * before it touches the output enable, and pinctrl-kona.c:44-55 gives in_bit 0 / out_bit 1.  The
 * first version of this code wrote only CFG and therefore never drove the pad at all: clearing OE
 * left it floating and setting OE drove whatever stale bit 1 was already there. */
static int
dbyTouchPadLevel(unsigned pin, int high)
{
	DbyTouchMap *tile = tlmmTile(pin);
	uint64_t block;
	uint32_t io;

	if (tile == nullptr || !tlmmBlock(pin, &block)) {
		return DBY_TOUCH_ERR_IO;
	}
	io = rd32(tile->base, block + DBY_TOUCH_TLMM_GPIO_IN);
	io = high ? (io | DBY_TOUCH_GPIO_OUT_BIT) : (io & ~DBY_TOUCH_GPIO_OUT_BIT);
	if (!wr32Checked(tile->base, block + DBY_TOUCH_TLMM_GPIO_IN, io, DBY_TOUCH_GPIO_OUT_BIT)) {
		return DBY_TOUCH_ERR_IO;
	}
	return DBY_TOUCH_OK;
}

/* Short register write used by the firmware upload: one page latch plus one data window, address
 * byte first.  dby_touch_n9z_write_block in the protocol layer is the same primitive, but that one
 * takes a bus object and the upload path already holds the engine, so this keeps one shape in the
 * file.  Larger payloads go through dbyTouchSramWrite, which re-latches the address and re-emits
 * the opcode byte for EVERY window and releases CS between them - see its comment. */
static int
dbyTouchPadProbeWrite(uint32_t addr, const uint8_t *bytes, size_t len)
{
	uint8_t latch[3] = { 0xFF, (uint8_t)((addr >> 15) & 0xFF), (uint8_t)((addr >> 7) & 0xFF) };
	uint8_t tx[16];

	if (len == 0 || len + 1 > sizeof tx) {
		return DBY_TOUCH_ERR_PARAM;
	}
	if (geni_spi_transfer(&gSpi, latch, NULL, sizeof latch, 0) != 0) {
		return DBY_TOUCH_ERR_IO;
	}
	tx[0] = (uint8_t)((addr & 0x7F) | 0x80);
	for (size_t i = 0; i < len; i++) {
		tx[1 + i] = bytes[i];
	}
	return geni_spi_transfer(&gSpi, tx, tx, len + 1, 0) == 0 ? DBY_TOUCH_OK : DBY_TOUCH_ERR_IO;
}

/* Drive the reset line: level first, then output enable, then verify both by read-back.  Order and
 * encoding from pinctrl-msm.c:440-473 and the vendor DT's reset states (function gpio, 8 mA,
 * bias-disable; huawei_tp.dtsi DTBO:9991-10025).  The OE bit stays set in both phases - the pad is
 * always driven, only its level changes - because a cleared OE means a floating pad, not "reset
 * asserted". */
static int
dbyTouchResetDrive(int high)
{
	uint32_t want = DBY_TOUCH_CFG_DRV_8MA | DBY_TOUCH_CFG_OE;
	uint32_t cfg = 0, io = 0;

	if (dbyTouchPadLevel(DBY_TOUCH_GPIO_RESET, high) != DBY_TOUCH_OK) {
		return DBY_TOUCH_ERR_IO;
	}
	if (dbyTouchPinWrite(DBY_TOUCH_GPIO_RESET, want,
	        DBY_TOUCH_CFG_MUX_MASK | DBY_TOUCH_CFG_PULL_MASK | DBY_TOUCH_CFG_DRV_MASK |
	        DBY_TOUCH_CFG_OE) != DBY_TOUCH_OK) {
		return DBY_TOUCH_ERR_IO;
	}
	if (dbyTouchPinRead(DBY_TOUCH_GPIO_RESET, &cfg, &io) != DBY_TOUCH_OK) {
		return DBY_TOUCH_ERR_IO;
	}
	if ((cfg & DBY_TOUCH_CFG_MUX_MASK) != 0 || (cfg & DBY_TOUCH_CFG_OE) == 0) {
		return DBY_TOUCH_ERR_IO;
	}
	if (((io & DBY_TOUCH_GPIO_OUT_BIT) != 0) != (high != 0)) {
		return DBY_TOUCH_ERR_IO;
	}
	return DBY_TOUCH_OK;
}

/* The state the chip must be left in: out of reset.  Every error path calls this, and so does the
 * end of the stage - a sequence that fails half way must not leave the panel dead until the next
 * power cycle. */
static void
dbyTouchResetRelease(void)
{
	(void)dbyTouchResetDrive(1);
	(void)dbyTouchPinRead(DBY_TOUCH_GPIO_RESET, &gResetCfg, &gResetIo);
}

/* Configure gpio39 as a plain GPIO input with the SoC pull-up the vendor DT asks for
 * (pmx_ts_int_active: function gpio, 8 mA, bias-pull-up), so its level can be watched without an
 * interrupt controller in the path.  OE is in the verify mask: leaving it out is how a pad ends up
 * an output by accident, which is the mistake applyPinConfig's own comment says was fixed there. */
static int
dbyTouchIrqPinConfig(void)
{
	uint32_t want = DBY_TOUCH_CFG_PULL_UP | DBY_TOUCH_CFG_DRV_8MA;

	return dbyTouchPinWrite(DBY_TOUCH_GPIO_IRQ, want,
	    DBY_TOUCH_CFG_MUX_MASK | DBY_TOUCH_CFG_PULL_MASK | DBY_TOUCH_CFG_DRV_MASK |
	    DBY_TOUCH_CFG_OE);
}

/* Sample gpio39 for `ms` milliseconds.  The vendor's interrupt is level-held (it stays asserted
 * until get_frame reads 0x20), so the LEVEL is the sound signal here and a low level during the
 * window is the interesting fact; the transition count only distinguishes "went low in this window"
 * from "was already low".  A 2 ms sample period cannot prove silence if the chip pulses instead of
 * holding - a pulse shorter than 2 ms would be missed - so edges == 0 with level == 1 is reported as
 * "no evidence of frames", not as "no frames". */
static void
dbyTouchSampleIrq(unsigned ms, uint32_t *level_out, uint32_t *edges_out)
{
	unsigned last = 1;
	uint32_t edges = 0;
	uint32_t io = 0;

	if (dbyTouchPinRead(DBY_TOUCH_GPIO_IRQ, nullptr, &io) == DBY_TOUCH_OK) {
		last = io & DBY_TOUCH_GPIO_IN_BIT;
	}
	if (level_out != nullptr) {
		*level_out = last;
	}
	for (unsigned waited = 0; waited < ms; waited += 2) {
		dbyBusDelay(nullptr, 2);
		if (dbyTouchPinRead(DBY_TOUCH_GPIO_IRQ, nullptr, &io) != DBY_TOUCH_OK) {
			break;
		}
		unsigned now = io & DBY_TOUCH_GPIO_IN_BIT;

		if (last == 1 && now == 0) {
			edges++;
		}
		last = now;
	}
	if (edges_out != nullptr) {
		*edges_out = edges;
	}
}

/* Read the four bytes at the counter address.  The bytes are published raw rather than decoded:
 * 0x3F8 is where this board's own frames show movement, but the offset is a hypothesis and a wrong
 * one must look wrong in the log, not like a clean zero.  dby_touch_n9z_read returns the whole
 * transfer, and the chip's first two bytes are the address phase, so data starts at peek[2]. */
static int
dbyTouchProbeCounter(struct dby_touch_bus *bus)
{
	uint8_t peek[32];

	if (dby_touch_n9z_read(bus, DBY_TOUCH_EVENT_BUF + DBY_TOUCH_FRAME_COUNTER, peek, sizeof peek) !=
	    DBY_TOUCH_OK) {
		gCounterBytes[0] = 0xFFu;
		gCounterBytes[1] = 0xFFu;
		gCounterBytes[2] = 0xFFu;
		gCounterBytes[3] = 0xFFu;
		return DBY_TOUCH_ERR_IO;
	}
	for (unsigned i = 0; i < 4; i++) {
		gCounterBytes[i] = peek[2 + i];
	}
	return DBY_TOUCH_OK;
}

/* Send one command in the record format this part's firmware parses, taken from the board's own
 * vendor kernel (novatek_thp.c:855-913, D_SEND_CMD_LEN = 7):
 *
 *   read the current firmware stamp, then
 *   latch the command address, then ONE 7-byte transfer:
 *     { (cmd_addr & 0x7F) | 0x80, cmd, 0xBB, arg_lo, arg_hi, stamp - 1, checksum }
 *   with checksum = two's complement of bytes 1..5,
 *   then poll the stamp until it equals what we wrote (5 tries, 1 ms apart), settle 50 ms.
 *
 * Two things this is NOT, both of which cost a flash cycle if misread:
 *   - it is not the NT36532E five-byte record {0xBF, cmd, 0, lo, hi}: that has no 0xBB handshake, no
 *     stamp and no checksum, so this firmware does not parse it.
 *   - the acknowledgement is not "byte 0 got cleared"; on N9Z it is the stamp coming back.
 *
 * Returns 0 = stamp matched, 1 = it never did within the retries, 2 = transfer error. */
static uint8_t
dbyTouchSendCommand(struct dby_touch_bus *bus, uint8_t cmd, uint8_t argLo, uint8_t argHi)
{
	uint8_t record[7];
	uint8_t stamp = 0;
	uint8_t want;

	/* r_buf[2] is "the valid value" for a three-byte read (novatek_thp.c:717-750); our read returns
	 * the same bytes with the two address-phase bytes first, so that is rx[2]. */
	if (dby_touch_n9z_read(bus, DBY_TOUCH_CMD_STAMP_ADDR, record, 3) != DBY_TOUCH_OK) {
		return 2;
	}
	stamp = record[2];
	want = (uint8_t)(stamp - 1u);

	record[0] = (uint8_t)((DBY_TOUCH_EVENT_BUF + DBY_TOUCH_EVENT_HOST_CMD) & 0x7F) | 0x80u;
	record[1] = cmd;
	record[2] = DBY_TOUCH_CMD_HANDSHAKE;
	record[3] = argLo;
	record[4] = argHi;
	record[5] = want;
	record[6] = (uint8_t)(~(uint8_t)(record[1] + record[2] + record[3] + record[4] + record[5]) + 1u);

	/* write_block re-derives the address byte from the address, so passing &record[1] with len 6
	 * sends exactly {cmd, 0xBB, arg_lo, arg_hi, stamp, checksum} after it. */
	if (dby_touch_n9z_write_block(bus, DBY_TOUCH_EVENT_BUF + DBY_TOUCH_EVENT_HOST_CMD,
	        &record[1], 6) != DBY_TOUCH_OK) {
		return 2;
	}
	for (unsigned attempt = 0; attempt < DBY_TOUCH_HOSTCMD_TRIES; attempt++) {
		dbyBusDelay(nullptr, DBY_TOUCH_CMD_RETRY_DELAY_MS);
		if (dby_touch_n9z_read(bus, DBY_TOUCH_CMD_STAMP_ADDR, record, 3) != DBY_TOUCH_OK) {
			return 2;
		}
		if (record[2] == want) {
			dbyBusDelay(nullptr, DBY_TOUCH_CMD_SETTLE_MS);
			return 0;
		}
	}
	return 1;
}

/* Write a short byte run at `addr` as one transfer (one latch + one data window).  Declared here
 * because the firmware upload above uses it and the pad helpers are defined later in the file. */
static int dbyTouchPadProbeWrite(uint32_t addr, const uint8_t *bytes, size_t len);

/* ------------------------------------------------------------------ */
/* Firmware upload (stage 8)                                          */
/* ------------------------------------------------------------------ */

/* The record this part's firmware needs before it will run anything, recovered from the board's own
 * vendor HAL: work/tracks/n9z-fw-upload/REPORT.md (every constant below is tied to an instruction
 * address there).  Summary of the flow the HAL performs on every start, which is what this follows:
 *
 *   w 0x3F0FE <- 0x69                 bootloader reset
 *   5 ms
 *   w 0x3F128 <- 00 00 00            ILM SRAM base   0x000000
 *   w 0x3F118 <- A3 46 01           ILM length      0x0146A3 (+1 when streaming)
 *   w 0x3F100 <- 0F 68 2A 98        ILM CRC         0x982A680F   [special framing]
 *   w 0x3F12C <- 00 00 02           DLM SRAM base   0x020000
 *   w 0x3F130 <- 47 BC 00           DLM length      0x00BC47 (+1)
 *   w 0x3F104 <- F0 0A 8E 68        DLM CRC         0x688E0AF0
 *   w 0x3F7E8 <- 0x69                tx auto-copy mode
 *   <stream ILM then DLM into SRAM>
 *   poll 0x3F7F1 == 0  (200 x 1 ms)  SPI DMA tx done
 *   w 0x3F30E |= 0x80                bootloader CRC enable
 *   w 0x2FE60 <- 0x00                clear fw state
 *   w 0x2FE50 <- 0xAE                run
 *   w 0x3F10D |= 0x01                boot-ready
 *   5 ms
 *   poll 0x2FE60..+5 until byte0 in 0xA0..0xAF  (10 x 10 ms)
 *   the whole attempt is repeated up to 3 times
 *
 * Two deliberate deviations, both because this driver is not the Huawei kernel:
 *   - an earlier revision of the upload report claimed the 0x3F100 CRC write takes a special
 *     two-byte-shifted path inside rwSPIBuf.  That claim was retracted (work/tracks/n9z-upload-recheck
 *     REPORT.md): the "special" mask is built by orr + movk without the lsl#16 the first reading
 *     assumed, so it covers only addr & 0x7F in {0x7F,0x20,0x21} and no address in this sequence
 *     (including 0x3F100 and SRAM 0x000000) is special.  The plain four-byte write below is right.
 *   - transfers are limited to DBY_TOUCH_FW_CHUNK bytes, but NOT by holding CS across them: every
 *     window re-latches the address and re-emits the opcode byte, with CS released in between, which
 *     is what rwSPIBuf does (it loops per piece).  Holding CS and relying on an auto-incrementing
 *     address pointer is the modelling error that made the first upload write almost every byte to
 *     the wrong SRAM address while all SPI transfers still reported success.  The 127+1 split keeps
 *     each data window at the 128 bytes this engine has been proven to carry.
 */

/* Window size for the SRAM stream.  The first version used 127 payload bytes because the 8208-byte
 * frame READ needed 128-byte chunks to stay under the engine's RX watermark; work/tracks/n9z-write-path
 * REPORT.md shows that ceiling is a host-side drain artifact, not a chip limit, and every reference
 * implementation streams in far larger windows (the vendor HAL uses 0x13FD = 5117 bytes, the open
 * drivers 63 KiB).  Framing is otherwise byte-identical to all of them. */
#define DBY_TOUCH_FW_CHUNK 0x13FDU

/* SRAM partitions, from the image header the HAL forwards verbatim (report §3.2/§7). */
struct dbyTouchFwPart {
	uint32_t bin_off;
	uint32_t sram;
	uint32_t len;
};

/* Four partitions, not two.  The image header carries two 0x100-byte blocks (at file 0x000 and
 * 0x100) that the vendor HAL ALSO streams - to SRAM 0x2BC48 and 0x2BD48, right after DLM, where
 * they sit contiguously with it (DLM SRAM 0x020000 + 0x0BC48 = 0x2BC48).  Their lengths are 0xFF
 * in the header and are streamed as SIZE+1 = 0x100, the same rule as ILM/DLM; their CRCs ride
 * inside the payload, not in a register.  Omitting them was a real defect: work/tracks/n9z-fw-image
 * REPORT.md verified the header layout and the CRC-32/MPEG-2 values over all four slices against
 * the binary, and the HAL streams sixteen entries (0x28664: w28 = 0x0E with the trailer flag set),
 * skipping only the zero-length ones. */
/* Build the four partitions from whichever image is selected.  The first two take their offset,
 * SRAM base, length and CRC from that image's own header (they differ per project); the last two
 * are structural - always 0x100 bytes at file 0x000/0x100, streamed right after DLM at 0x2BC48 -
 * so they stay literal.  Nothing is cached: the picker may repoint gDbyTouchFw between uploads. */
static void
dbyTouchFwBuildParts(struct dbyTouchFwPart *out)
{
	const struct dbyTouchFwSel *fw = gDbyTouchFw;

	out[0].bin_off = fw->ilm_off;
	out[0].sram = fw->ilm_sram;
	out[0].len = fw->ilm_len + 1;      /* streamed SIZE+1 */
	out[1].bin_off = fw->dlm_off;
	out[1].sram = fw->dlm_sram;
	out[1].len = fw->dlm_len + 1;      /* streamed SIZE+1 */
	out[2].bin_off = 0x00000;          /* header block A: length 0xFF, streamed +1 */
	out[2].sram = 0x02BC48;
	out[2].len = 0x000100;
	out[3].bin_off = 0x00100;          /* header block B: length 0xFF, streamed +1 */
	out[3].sram = 0x02BD48;
	out[3].len = 0x000100;
}

/* Stream a partition into SRAM the way the HAL's rwSPIBuf actually does it.
 *
 * THE BUG THIS REPLACES: the first version sent one page latch per partition, one opcode byte in
 * the first window, and then raw bytes with CS held for the rest.  rwSPIBuf does not work that way -
 * it loops per piece (0x15d84) and re-emits BOTH the three-byte latch (0x15bdc) and the opcode
 * (addr & 0x7F) | 0x80 (0x15c6c) for EVERY piece, each as its own CS window.  Holding CS and relying
 * on an auto-incrementing address pointer therefore writes every byte after the first window to the
 * wrong address: the image never lands, the chip never boots, and every SPI transfer still reports
 * success.  That is exactly the "up=29 with everything OK" signature this round produced.
 *
 * So: per window, latch (addr + off), then one data window that starts with the opcode byte and
 * carries at most DBY_TOUCH_FW_CHUNK - 1 payload bytes, so no single transfer exceeds the 128 bytes
 * this engine has been proven to carry.  CS is released between windows, as the HAL does (it has no
 * keep-CS concept at all).
 */
static int
dbyTouchSramWrite(uint32_t addr, const uint8_t *data, size_t len)
{
	/* DBY_TOUCH_FW_CHUNK + 1 bytes for the opcode byte; far too large for the stack in a kernel
	 * thread, so it is allocated for the duration of one partition. */
	uint8_t *tx = (uint8_t *)IOMallocAligned(DBY_TOUCH_FW_CHUNK + 1, 64);
	size_t done = 0;
	int rc = DBY_TOUCH_OK;

	if (tx == nullptr) {
		return DBY_TOUCH_ERR_IO;
	}

	while (done < len) {
		uint32_t at = addr + (uint32_t)done;
		uint8_t latch[3];
		size_t n = len - done;

		if (n > DBY_TOUCH_FW_CHUNK - 1) {
			n = DBY_TOUCH_FW_CHUNK - 1;
		}
		latch[0] = 0xFF;
		latch[1] = (uint8_t)((at >> 15) & 0xFF);
		latch[2] = (uint8_t)((at >> 7) & 0xFF);
		if (geni_spi_transfer(&gSpi, latch, NULL, sizeof latch, 0) != 0) {
			rc = DBY_TOUCH_ERR_IO;
			break;
		}
		tx[0] = (uint8_t)((at & 0x7F) | 0x80);
		for (size_t i = 0; i < n; i++) {
			tx[1 + i] = data[done + i];
		}
		if (geni_spi_transfer(&gSpi, tx, tx, n + 1, 0) != 0) {
			rc = DBY_TOUCH_ERR_IO;
			break;
		}
		done += n;
		/* Publish progress every 64 windows: the log writer re-composes the page every 2 s, and
		 * pulishPageRow() is what refreshes these rows. */
		if ((++gUploadWindows & 31u) == 0u) {
			publishPageRow();
		}
	}
	IOFreeAligned(tx, DBY_TOUCH_FW_CHUNK + 1);
	return rc;
}

static int
dbyTouchFwWrite(struct dby_touch_bus *bus, uint32_t addr, const uint8_t *bytes, size_t len)
{
	(void)bus;
	return dbyTouchSramWrite(addr, bytes, len);
}

/* One full attempt.  Returns DBY_TOUCH_OK when the chip reports a booted firmware. */
static int
dbyTouchFwAttemptOnce(struct dby_touch_bus *bus)
{
	static const uint8_t kBootReset = 0x69;
	static const uint8_t kIlmBase[3] = { 0x00, 0x00, 0x00 };
	/* Lengths and CRCs come from the image header in DbyTouchFw.h, so replacing the firmware array
	 * is the whole of a project change.  A stale value here does not fail loudly: the stream reports
	 * success and the chip rejects the image, which reads as "uploaded fine, still no touch". */
	const uint8_t kIlmLen[3] = {
		(uint8_t)(gDbyTouchFw->ilm_len & 0xFF),
		(uint8_t)((gDbyTouchFw->ilm_len >> 8) & 0xFF),
		(uint8_t)((gDbyTouchFw->ilm_len >> 16) & 0xFF) };
	const uint8_t kIlmCrc[4] = {
		(uint8_t)(gDbyTouchFw->ilm_crc & 0xFF),
		(uint8_t)((gDbyTouchFw->ilm_crc >> 8) & 0xFF),
		(uint8_t)((gDbyTouchFw->ilm_crc >> 16) & 0xFF),
		(uint8_t)((gDbyTouchFw->ilm_crc >> 24) & 0xFF) };
	static const uint8_t kDlmBase[3] = { 0x00, 0x00, 0x02 };
	const uint8_t kDlmLen[3] = {
		(uint8_t)(gDbyTouchFw->dlm_len & 0xFF),
		(uint8_t)((gDbyTouchFw->dlm_len >> 8) & 0xFF),
		(uint8_t)((gDbyTouchFw->dlm_len >> 16) & 0xFF) };
	const uint8_t kDlmCrc[4] = {
		(uint8_t)(gDbyTouchFw->dlm_crc & 0xFF),
		(uint8_t)((gDbyTouchFw->dlm_crc >> 8) & 0xFF),
		(uint8_t)((gDbyTouchFw->dlm_crc >> 16) & 0xFF),
		(uint8_t)((gDbyTouchFw->dlm_crc >> 24) & 0xFF) };
	struct dbyTouchFwPart parts[4];
	dbyTouchFwBuildParts(parts);
	IOLog("DbyTouch(upload): image=%s ilm=%05x/%08x dlm=%05x/%08x\n",
	    gDbyTouchFw->project, (unsigned)gDbyTouchFw->ilm_len, (unsigned)gDbyTouchFw->ilm_crc,
	    (unsigned)gDbyTouchFw->dlm_len, (unsigned)gDbyTouchFw->dlm_crc);
	uint8_t v;
	uint8_t st[6];

	/* Steps 1-9. */
	if (dbyTouchPadProbeWrite(DBY_TOUCH_N9Z_BOOT_ADDR, &kBootReset, 1) != DBY_TOUCH_OK) {
		return 1;
	}
	dbyBusDelay(nullptr, 5);
	if (dbyTouchPadProbeWrite(0x3F128, kIlmBase, sizeof kIlmBase) != DBY_TOUCH_OK) {
		return 2;
	}
	if (dbyTouchPadProbeWrite(0x3F118, kIlmLen, sizeof kIlmLen) != DBY_TOUCH_OK) {
		return 3;
	}
	if (dbyTouchPadProbeWrite(0x3F100, kIlmCrc, sizeof kIlmCrc) != DBY_TOUCH_OK) {
		return 4;
	}
	if (dbyTouchPadProbeWrite(0x3F12C, kDlmBase, sizeof kDlmBase) != DBY_TOUCH_OK) {
		return 5;
	}
	if (dbyTouchPadProbeWrite(0x3F130, kDlmLen, sizeof kDlmLen) != DBY_TOUCH_OK) {
		return 6;
	}
	if (dbyTouchPadProbeWrite(0x3F104, kDlmCrc, sizeof kDlmCrc) != DBY_TOUCH_OK) {
		return 7;
	}
	if (dbyTouchPadProbeWrite(0x3F7E8, &kBootReset, 1) != DBY_TOUCH_OK) {
		return 8;
	}

	/* Step 10: the SRAM partitions, in header order (ILM, DLM, then the two header blocks). */
	for (unsigned p = 0; p < 4; p++) {
		const struct dbyTouchFwPart *part = &parts[p];

		if (part->bin_off + part->len > gDbyTouchFw->len) {
			return 9;
		}
		if (dbyTouchFwWrite(bus, part->sram, &gDbyTouchFw->image[part->bin_off], part->len) !=
		    DBY_TOUCH_OK) {
			return 10 + (int)p;
		}
	}

	/* Step 11: the SPI DMA queue must drain before the chip is told to run. */
	for (unsigned i = 0; i < 200; i++) {
		if (dby_touch_n9z_read(bus, 0x3F7F1, st, 3) != DBY_TOUCH_OK) {
			return 20;
		}
		if (st[2] == 0x00u) {
			break;
		}
		if (i == 199) {
			return 21;
		}
		dbyBusDelay(nullptr, 1);
	}

	/* Steps 12-15. */
	if (dby_touch_n9z_read(bus, 0x3F30E, st, 3) != DBY_TOUCH_OK) {
		return 22;
	}
	v = (uint8_t)(st[2] | 0x80u);
	if (dbyTouchPadProbeWrite(0x3F30E, &v, 1) != DBY_TOUCH_OK) {
		return 23;
	}
	v = 0x00;
	if (dbyTouchPadProbeWrite(0x2FE60, &v, 1) != DBY_TOUCH_OK) {
		return 24;
	}
	v = 0xAE;
	if (dbyTouchPadProbeWrite(0x2FE50, &v, 1) != DBY_TOUCH_OK) {
		return 25;
	}
	if (dby_touch_n9z_read(bus, 0x3F10D, st, 3) != DBY_TOUCH_OK) {
		return 26;
	}
	v = (uint8_t)(st[2] | 0x01u);
	if (dbyTouchPadProbeWrite(0x3F10D, &v, 1) != DBY_TOUCH_OK) {
		return 27;
	}
	dbyBusDelay(nullptr, 5);

	/* Steps 18: the chip's own "firmware finished resetting" range. */
	for (unsigned i = 0; i < 10; i++) {
		if (dby_touch_n9z_read(bus, DBY_TOUCH_FW_STATE_ADDR, st, sizeof st) != DBY_TOUCH_OK) {
			return 28;
		}
		gUploadState = st[2];
		if (st[2] >= DBY_TOUCH_FW_STATE_OK_LO && st[2] <= DBY_TOUCH_FW_STATE_OK_HI) {
			return DBY_TOUCH_OK;
		}
		dbyBusDelay(nullptr, 10);
	}

	/* It did not boot.  Read (never write) the registers that say WHY, so the next round has
	 * evidence instead of another identical timeout. */
	{
		uint8_t b1[3];
		uint32_t golden[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
		uint32_t computed[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
		bool readOk[2] = { false, false };

		if (dby_touch_n9z_read(bus, 0x3F133, b1, sizeof b1) == DBY_TOUCH_OK) {
			gCrcFlags = b1[2] & 0x07u;
		}
		if (dby_touch_n9z_read(bus, 0x3F7E8, b1, sizeof b1) == DBY_TOUCH_OK) {
			gAutoCopy = b1[2];
		}
		for (unsigned k = 0; k < 2; k++) {
			uint8_t g4[6], c4[6];
			uint32_t gaddr = k == 0 ? 0x3F100u : 0x3F104u;
			uint32_t caddr = k == 0 ? 0x3F120u : 0x3F124u;

			if (dby_touch_n9z_read(bus, gaddr, g4, 6) == DBY_TOUCH_OK) {
				golden[k] = (uint32_t)g4[2] | ((uint32_t)g4[3] << 8) |
				            ((uint32_t)g4[4] << 16) | ((uint32_t)g4[5] << 24);
				gCrcGolden[k] = golden[k];
			}
			if (dby_touch_n9z_read(bus, caddr, c4, 6) == DBY_TOUCH_OK) {
				computed[k] = (uint32_t)c4[2] | ((uint32_t)c4[3] << 8) |
				              ((uint32_t)c4[4] << 16) | ((uint32_t)c4[5] << 24);
				gCrcComputed[k] = computed[k];
				readOk[k] = (golden[k] != 0xFFFFFFFFu);
			}
		}
		/* A pair whose two reads did not both succeed says nothing: comparing the 0xFFFFFFFF
		 * sentinel against itself would report a MATCH, which is the opposite of the truth and
		 * exactly the kind of false record this board has been burned by.  Any unreadable pair
		 * turns the whole count into "unknown" (printed as -). */
		if (readOk[0] && readOk[1]) {
			gCrcMismatch = 0;
			if (golden[0] != computed[0]) {
				gCrcMismatch++;
			}
			if (golden[1] != computed[1]) {
				gCrcMismatch++;
			}
		} else {
			gCrcMismatch = 0xFFFFFFFFu;
		}
		IOLog("DbyTouch(upload): crc133=%u autocopy=%u golden=%08x/%08x computed=%08x/%08x\n",
		    gCrcFlags, gAutoCopy, golden[0], golden[1], computed[0], computed[1]);

		/* Did the bytes land?  Read back the first 32 bytes of each partition's SRAM and compare
		 * with the image slice that was streamed there. */
		for (unsigned k = 0; k < 2; k++) {
			uint8_t probe[34];
			const struct dbyTouchFwPart *part = &parts[k];
			uint32_t diff = 0;

			gSramDiff[k] = 0xFFFFFFFFu;
			if (dby_touch_n9z_read(bus, part->sram, probe, 32) != DBY_TOUCH_OK) {
				continue;
			}
			for (unsigned i = 0; i < 30; i++) {
				if (probe[2 + i] != gDbyTouchFw->image[part->bin_off + i]) {
					if (diff == 0) {
						gSramFirstBad = i;
					}
					diff++;
				}
			}
			gSramDiff[k] = diff;
			gSramSampled = 30;
		}
	}
	return 29;
}

/* ---- Firmware selection: ask the device instead of guessing --------------------------------------
 *
 * DbyTouchFw.h now carries three builds for three panels (W2599Z1100 / Z1300 / Z2700).  The tablet
 * records which panel it has in its production-information partitions: the Android bootloader reads
 * that to pick the panel's DSI init sequence, so the project id has to be there.  We read the GPT,
 * find `oeminfo`, scan it for "W2599Z", and point gDbyTouchFw at the matching build BEFORE the
 * uploader runs - one boot, no trial upload, no reboot.  Nothing is written; a miss keeps the
 * built-in default (this board's own project, the last entry in the table).
 */
extern "C" int native_ufs_abs_read(uint64_t lba, uint32_t blocks, void *dst) __attribute__((weak_import));

static uint32_t
dbyTouchLe32(const uint8_t *at)
{
	return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

static uint64_t
dbyTouchLe64(const uint8_t *at)
{
	return (uint64_t)dbyTouchLe32(at) | ((uint64_t)dbyTouchLe32(at + 4) << 32);
}

/* Find the project id in one buffer.  "W2599Z" is the shared prefix; the four digits after it are
 * what tells the three builds apart. */
static bool
dbyTouchScanProject(const uint8_t *buf, size_t len, char *out, size_t outLen)
{
	static const char kMark[] = "W2599Z";
	const size_t markLen = sizeof kMark - 1;

	if (outLen < 5) {
		return false;
	}
	for (size_t i = 0; i + markLen + 4 <= len; i++) {
		if (buf[i] != 'W' || memcmp(buf + i, kMark, markLen) != 0) {
			continue;
		}
		const uint8_t *d = buf + i + markLen;
		if (d[0] < '0' || d[0] > '9' || d[1] < '0' || d[1] > '9' ||
		    d[2] < '0' || d[2] > '9' || d[3] < '0' || d[3] > '9') {
			continue;
		}
		memcpy(out, d, 4);
		out[4] = '\0';
		return true;
	}
	return false;
}

static bool
dbyTouchScanPartition(uint64_t first, uint64_t last, uint8_t *buf, uint64_t chunkBlocks,
    char *out, size_t outLen)
{
	if (last < first) {
		return false;
	}
	for (uint64_t lba = first; lba <= last; lba += chunkBlocks) {
		uint64_t left = last - lba + 1;
		uint32_t chunk = (uint32_t)(left < chunkBlocks ? left : chunkBlocks);
		if (!native_ufs_abs_read(lba, chunk, buf)) {
			return false;
		}
		if (dbyTouchScanProject(buf, (size_t)chunk * 4096, out, outLen)) {
			return true;
		}
	}
	return false;
}

static void
dbyTouchPickFirmwareFromDevice(void)
{
	static const uint64_t kChunkBlocks = 32;        /* 128 KiB: large enough to be fast, small enough to always allocate */
	const size_t bufBytes = (size_t)kChunkBlocks * 4096;
	uint64_t oemFirst = 0, oemLast = 0;
	char found[8] = { 0 };
	const char *src = "default";
	uint8_t *buf = nullptr;

	if (!&native_ufs_abs_read) {
		IOLog("DbyTouch(fwsel): no UFS reader linked, keeping %s\n", gDbyTouchFw->project);
		return;
	}
	buf = (uint8_t *)IOMallocAligned(bufBytes, 4096);
	if (!buf) {
		IOLog("DbyTouch(fwsel): no buffer, keeping %s\n", gDbyTouchFw->project);
		return;
	}

	/* The UFS driver may still be starting: give it a moment instead of losing the only chance. */
	bool gotGpt = false;
	for (unsigned attempt = 0; attempt < 200; attempt++) {
		if (native_ufs_abs_read(1, 1, buf)) {
			gotGpt = true;
			break;
		}
		dbyBusDelay(nullptr, 10);
	}
	if (!gotGpt || memcmp(buf, "EFI PART", 8) != 0) {
		IOLog("DbyTouch(fwsel): no GPT at LBA1, keeping %s\n", gDbyTouchFw->project);
		IOFreeAligned(buf, bufBytes);
		return;
	}

	{
		uint64_t entLba = dbyTouchLe64(buf + 72);
		uint32_t entCount = dbyTouchLe32(buf + 80);
		uint32_t entSize = dbyTouchLe32(buf + 84);
		uint64_t entBlocks = ((uint64_t)entCount * 128 + 4095) / 4096;

		if (entSize != 128 || entCount == 0 || entCount > 256 || entBlocks > kChunkBlocks) {
			IOLog("DbyTouch(fwsel): odd GPT (cnt=%u size=%u), keeping %s\n",
			    entCount, entSize, gDbyTouchFw->project);
			IOFreeAligned(buf, bufBytes);
			return;
		}
		if (!native_ufs_abs_read(entLba, (uint32_t)entBlocks, buf)) {
			IOLog("DbyTouch(fwsel): GPT entries unreadable, keeping %s\n", gDbyTouchFw->project);
			IOFreeAligned(buf, bufBytes);
			return;
		}
		for (uint32_t i = 0; i < entCount; i++) {
			const uint8_t *e = buf + (size_t)i * 128;
			char name[13];
			unsigned c = 0;

			for (; c < 12; c++) {
				uint8_t lo = e[56 + 2 * c], hi = e[57 + 2 * c];
				if (hi || !lo) {
					break;
				}
				name[c] = (char)lo;
			}
			name[c] = '\0';
			if (strcmp(name, "oeminfo") == 0) {
				oemFirst = dbyTouchLe64(e + 32);
				oemLast = dbyTouchLe64(e + 40);
				break;
			}
		}
	}

	if (oemLast < oemFirst) {
		IOLog("DbyTouch(fwsel): no oeminfo partition, keeping %s\n", gDbyTouchFw->project);
	} else if (dbyTouchScanPartition(oemFirst, oemLast, buf, kChunkBlocks, found, sizeof found)) {
		IOLog("DbyTouch(fwsel): oeminfo 0x%llx..0x%llx reports %s\n", oemFirst, oemLast, found);
		if (dbyTouchFwSelect(found) != 0) {
			IOLog("DbyTouch(fwsel): %s is not in the table, keeping %s\n", found, gDbyTouchFw->project);
		} else {
			src = "oeminfo";
		}
	} else {
		IOLog("DbyTouch(fwsel): oeminfo 0x%llx..0x%llx has no project id, keeping %s\n",
		    oemFirst, oemLast, gDbyTouchFw->project);
	}

	IOLog("DbyTouch(fwsel): image %s, ilm=%05x/%08x, source %s\n",
	    gDbyTouchFw->project, (unsigned)gDbyTouchFw->ilm_len, (unsigned)gDbyTouchFw->ilm_crc, src);
	snprintf(native_touch_rows[2], sizeof native_touch_rows[2],
	    "FW %s ilm=%05x src=%s", gDbyTouchFw->project, (unsigned)gDbyTouchFw->ilm_len, src);
	IOFreeAligned(buf, bufBytes);
}

/* Up to three attempts, as the HAL does. */
static void
dbyTouchUploadFirmware(struct dby_touch_bus *bus)
{
	int rc = DBY_TOUCH_ERR_IO;

	gUploadState = 0xFFFFFFFFu;
	gUploadWindows = 0;
	gUploadWindowsTotal = ((0x0146A4u + 126u) / 127u) + ((0x00BC48u + 126u) / 127u) +
	                      ((0x000100u + 126u) / 127u) + ((0x000100u + 126u) / 127u);
	gUploadRc = 0xFFFFFFFFu;
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		gUploadAttempts = attempt + 1;
		rc = dbyTouchFwAttemptOnce(bus);
		if (rc == DBY_TOUCH_OK) {
			break;
		}
	}
	gUploadRc = (rc == DBY_TOUCH_OK) ? 0u : (uint32_t)rc;
	IOLog("DbyTouch(upload): rc=%u attempts=%u state=%u\n", gUploadRc, gUploadAttempts, gUploadState);
}

/* The three commands sent at stage 7, in order:
 *   0  BT stylus status (0xC2)  - the vendor kernel's own boot command, used to prove that the
 *                                 mailbox write path and the stamp acknowledgement work at all;
 *   1  idle baseline (0x19)     - first half of the official start-scanning pair;
 *   2  active mode (0x01/0x0002)- second half.
 * If command 0 does not get its stamp back, the record format is wrong and nothing can be concluded
 * from 1 and 2; if only 0 succeeds, the opcodes are the open question. */
static void
dbyTouchStartScanning(struct dby_touch_bus *bus)
{
	gHostCmdRc[0] = dbyTouchSendCommand(bus, DBY_TOUCH_CMD_BT_STATUS, 0x02u, 0x00u);
	gHostCmdRc[1] = dbyTouchSendCommand(bus, DBY_TOUCH_HOSTCMD_IDLE, 0x00u, 0x00u);
	gHostCmdRc[2] = dbyTouchSendCommand(bus, DBY_TOUCH_HOSTCMD_ACTIVE, 0x02u, 0x00u);
	IOLog("DbyTouch(hostcmd): bt=%u idle=%u active=%u\n",
	    gHostCmdRc[0], gHostCmdRc[1], gHostCmdRc[2]);
}

/* One enable round.  Returns DBY_TOUCH_OK only when every step answered; gEnableRc names the step
 * that stopped, and the reset line is released on every exit. */
static int
dbyTouchChipEnable(struct dby_touch_bus *bus)
{
	struct dby_touch_identify id;
	int rc;

	gEnableRan = true;
	gEnableRc = 0;
	gIrqLevel = 0xFFFFFFFFu;
	gIrqEdges = 0;
	gEnableIdentRc = 0xFFFFFFFFu;
	gEnableIdentRow = -1;
	gCounterBytes[0] = 0xFFu;
	gCounterBytes[1] = 0xFFu;
	gCounterBytes[2] = 0xFFu;
	gCounterBytes[3] = 0xFFu;

	/* Pin 39 first: it is read after the release, and configuring it later would make the first
	 * samples meaningless. */
	if (dbyTouchIrqPinConfig() != DBY_TOUCH_OK) {
		gEnableRc = 1;
		return DBY_TOUCH_ERR_IO;
	}

	/* The vendor's reset pulse (novatek_thp.c:317-321, same shape in resume :566-569):
	 *   line LOW -> write 0xAA -> line HIGH
	 * The low phase is not 0 ms in practice: touch_driver_sw_reset_idle ends with a 10 ms delay
	 * (novatek_thp.c:249).  5 ms + write + 10 ms is the same or longer, which is the safe side. */
	if (dbyTouchResetDrive(0) != DBY_TOUCH_OK) {
		gEnableRc = 2;
		dbyTouchResetRelease();
		return DBY_TOUCH_ERR_IO;
	}
	dbyBusDelay(nullptr, 5);

	/* 0xAA while the line is asserted takes the chip out of its bootloader and back to idle; 0x69
	 * does the opposite and is what the identify pass sends first. */
	if (dby_touch_n9z_write(bus, DBY_TOUCH_N9Z_BOOT_ADDR, DBY_TOUCH_N9Z_IDLE_BYTE) != DBY_TOUCH_OK) {
		gEnableRc = 3;
		dbyTouchResetRelease();
		return DBY_TOUCH_ERR_IO;
	}
	dbyBusDelay(nullptr, 10);

	if (dbyTouchResetDrive(1) != DBY_TOUCH_OK) {
		gEnableRc = 4;
		dbyTouchResetRelease();
		return DBY_TOUCH_ERR_IO;
	}
	dbyBusDelay(nullptr, 10);

	/* The vendor never trusts the state after a hard reset: every identity round is 0x69 -> 0xAA
	 * -> read id (novatek_thp.c:361-362).  A chip that came out of reset in its bootloader would
	 * return garbage from the 0x20 frame read and be misread as "the reset did not help". */
	if (dby_touch_n9z_write(bus, DBY_TOUCH_N9Z_BOOT_ADDR, DBY_TOUCH_N9Z_IDLE_BYTE) != DBY_TOUCH_OK) {
		gEnableRc = 5;
		dbyTouchResetRelease();
		return DBY_TOUCH_ERR_IO;
	}
	dbyBusDelay(nullptr, 10);
	rc = dby_touch_identify_n9z(bus, &id);
	gEnableIdentRc = (rc == DBY_TOUCH_OK) ? 0u : ((rc == DBY_TOUCH_ERR_PROTO) ? 2u : 3u);
	gEnableIdentRow = id.trim_row;
	if (rc != DBY_TOUCH_OK) {
		gEnableRc = 6;
		dbyTouchResetRelease();
		return DBY_TOUCH_ERR_PROTO;
	}

	/* Firmware liveness, before any command is sent: six bytes at 0x2FE60, first data byte.  The
	 * vendor HAL treats 0xA0..0xAF as "firmware finished resetting".  On a no-flash part with no
	 * image this reads something else, and that single byte decides whether the rest of this round
	 * is even meaningful. */
	{
		uint8_t fw[6];

		if (dby_touch_n9z_read(bus, DBY_TOUCH_FW_STATE_ADDR, fw, sizeof fw) == DBY_TOUCH_OK) {
			gFwState = fw[2];
		}
	}

	/* Stage 8: upload the firmware image the vendor HAL loads on every start, then re-read the same
	 * state byte.  The probe above is what says whether this is needed; the upload is attempted
	 * regardless of its value only when stage 8 was asked for, so stage 7 still costs nothing. */
	if (gState.stage >= DBY_TOUCH_STAGE_FWUPLOAD) {
		static bool picked = false;
		if (!picked) {
			picked = true;
			dbyTouchPickFirmwareFromDevice();
		}
		dbyTouchUploadFirmware(bus);
		if (gUploadRc == 0) {
			uint8_t fw[6];

			if (dby_touch_n9z_read(bus, DBY_TOUCH_FW_STATE_ADDR, fw, sizeof fw) == DBY_TOUCH_OK) {
				gFwState = fw[2];
			}
		}
	}

	/* Two probes one sampling window apart: if the chip started scanning, the counter at 0x3F8
	 * moves and gpio39 shows activity in between. */
	if (dbyTouchProbeCounter(bus) != DBY_TOUCH_OK) {
		gEnableRc = 7;
		dbyTouchResetRelease();
		return DBY_TOUCH_ERR_IO;
	}
	/* Stage 7: the reset release alone left the chip answering its registers but returning an
	 * 8192-byte uniform-random block instead of a report page (measured 2026-11-02 06:30), which is
	 * the shape of "reset done, reporting never started".  The start-reporting command lives in the
	 * closed daemon, not in the vendor kernel, so it is sent here by the official driver's protocol.
	 * It runs between the two counter probes so a change is visible in f=. */
	if (gState.stage >= DBY_TOUCH_STAGE_HOSTCMD) {
		for (unsigned i = 0; i < 4; i++) {
			gCounterFirst[i] = gCounterBytes[i];
		}
		dbyTouchStartScanning(bus);
		if (dbyTouchProbeCounter(bus) != DBY_TOUCH_OK) {
			gCounterBytes[0] = 0xFFu;
			gCounterBytes[1] = 0xFFu;
			gCounterBytes[2] = 0xFFu;
			gCounterBytes[3] = 0xFFu;
			/* A probe that failed is a step that did not answer, not a success. */
			gEnableRc = 8;
		}
		gCounterMoved = (gCounterFirst[0] != gCounterBytes[0] ||
		                 gCounterFirst[1] != gCounterBytes[1] ||
		                 gCounterFirst[2] != gCounterBytes[2] ||
		                 gCounterFirst[3] != gCounterBytes[3]);
	}
	dbyTouchSampleIrq(200, &gIrqLevel, &gIrqEdges);
	dbyTouchResetRelease();
	IOLog("DbyTouch(enable): rc=0 ident=%u/%d irq=%u/%u f=%02x%02x%02x%02x rst_cfg=%08x rst_io=%08x\n",
	    gEnableIdentRc, gEnableIdentRow, gIrqLevel, gIrqEdges,
	    gCounterBytes[0], gCounterBytes[1], gCounterBytes[2], gCounterBytes[3],
	    gResetCfg, gResetIo);
	return DBY_TOUCH_OK;
}

/* ------------------------------------------------------------------ */
/* Contact extraction (in kernel)                                     */
/* ------------------------------------------------------------------ */

/* Turn one 8208-byte frame into a single contact.
 *
 * Calibration, measured on hardware 2026-11-02 and re-derived by work/verify/corner-calibration.py
 * from evidence/edl-frames40/frames40.img (sha256 B47590FD...):
 *   - payload is rx[2..8193], 16-bit little-endian, OFFSET BINARY: resting is 0x8000 and a finger
 *     RAISES the value.  It is not two's complement.  Reading it as int16_t put half of every frame
 *     at the opposite end of the range and made the old coherence gate reject 40 of 40 real frames
 *     (review #2, D1: measured coherence 54.8 %..63.1 %);
 *   - the first two words are a header, so the matrix starts at payload offset 4;
 *   - the matrix is 50 rows x 32 columns (node = row * 32 + col; FWINFO's x_num is the COLUMN count),
 *     row 0 the top edge, column 0 the left edge, no rotation, no mirroring;
 *   - the per-node resting level is NOT flat: the top-left cluster rests up to 3900 below the
 *     frame-wide median, so a full-strength press there still lands below it and the corner was never
 *     detected (review #2, D13).  Hence a per-node reference;
 *   - x = (col + 0.5) * 1600 / 32, y = (row + 0.5) * 2560 / 50, panel size from DbyTouchBoard.h.
 */

/* Per-node resting reference: the lowest value each node has reported since the reader started.  A
 * press only ever raises a node, so a minimum is a finger-free reference that also heals itself the
 * moment a held finger lifts - no decay term, no frame history to allocate.  Seeded from the first
 * frame handed over, which costs exactly one frame of blindness.
 *
 * 1600 nodes = 3200 bytes of BSS. */
static uint16_t gTouchBase[DBY_TOUCH_MATRIX_ROWS * DBY_TOUCH_MATRIX_COLS];
static bool     gTouchBaseSeeded;

/* Node n of the matrix, as the offset-binary word the chip reports. */
static uint16_t
dbyTouchNode(const uint8_t *frame, unsigned n)
{
	uint16_t raw;

	__builtin_memcpy(&raw, frame + 2 + DBY_TOUCH_MATRIX_HDR + (size_t)n * 2, sizeof raw);
	return raw;
}

static void
dbyTouchSeedBase(const uint8_t *frame)
{
	const unsigned nodes = DBY_TOUCH_MATRIX_ROWS * DBY_TOUCH_MATRIX_COLS;
	unsigned n;

	for (n = 0; n < nodes; n++) {
		gTouchBase[n] = dbyTouchNode(frame, n);
	}
	gTouchBaseSeeded = true;
}

/* Connected-component scratch (review M1, 2026-11-03).  1600 entries each: the per-node delta above
 * its reference, a flood-fill "seen" generation, and the flood stack.  uint16 is enough for all three
 * because the delta is clamped to DBY_TOUCH_TOUCH_CEILING (8000) and a node index is < 1600. */
static uint16_t gTouchDelta[DBY_TOUCH_MATRIX_ROWS * DBY_TOUCH_MATRIX_COLS];
static uint16_t gTouchSeen[DBY_TOUCH_MATRIX_ROWS * DBY_TOUCH_MATRIX_COLS];
static uint16_t gTouchStack[DBY_TOUCH_MATRIX_ROWS * DBY_TOUCH_MATRIX_COLS];
static uint16_t gTouchGen;

/* Next flood generation.  One flood per component (up to DBY_TOUCH_MAX_CANDIDATES) plus one per accepted contact advances the counter several times per frame
 * winner), so the counter advances twice; a wrap would alias onto marks left in gTouchSeen, so the
 * wrap clears the array and restarts at 1.  No memset: this file does not use it anywhere. */
static uint16_t
dbyTouchGenNext(void)
{
	if (++gTouchGen == 0u) {
		unsigned i;

		for (i = 0; i < (unsigned)(sizeof gTouchSeen / sizeof gTouchSeen[0]); i++) {
			gTouchSeen[i] = 0u;
		}
		++gTouchGen;
	}
	return gTouchGen;
}

/* Flood one 4-connected component of nodes whose delta is non-zero, starting at seed.  Returns the
 * component's node count and, when asked, its weighted sums.
 *
 * 4-connectivity is the whole point (review M1).  With the 2026-11-03 floor at 700 the untouched
 * 791-second recording (evidence/edl-frames40-2230/frames40.img, sha256 cc062822...) puts 2..4 nodes
 * above the floor in 13 of 39 frames whenever a reference step lands at frame 10, and those nodes are
 * never 4-adjacent to each other - frame 18 has (r0,c30), (r18,c14), (r25,c29), (r20,c0), i.e. 0 of 4
 * adjacent.  Summing them (the pre-review code) published phantom contacts at 775,1281 -> 800,519 ->
 * 1025,760 within 30 s of an untouched boot.  Every real press in every recording is one contiguous
 * patch (2 of 2 adjacent in the 20-frame sweep, 5..9-node patches elsewhere), so gating on
 * connectivity keeps the sensitivity that the lower floor bought and drops the phantoms. */
static unsigned
dbyTouchFlood(unsigned seed, const uint16_t *delta, uint16_t gen, unsigned cols, unsigned rows,
    uint64_t *wsum, uint64_t *wx, uint64_t *wy, uint32_t *pmax, uint32_t weightFloor)
{
	unsigned head = 0, tail = 0, count = 0;

	gTouchStack[tail++] = (uint16_t)seed;
	gTouchSeen[seed] = gen;
	while (head < tail) {
		const unsigned cur = gTouchStack[head++];
		const unsigned r = cur / cols;
		const unsigned c = cur % cols;
		const uint32_t d = delta[cur];
		uint32_t w;

		if (pmax != nullptr && d > *pmax) {
			*pmax = d;
		}
		/* Weight = delta clamped to the ceiling, MINUS a floor, never negative.
		 *
		 * 2026-11-03 (touch-precision study, following the author's rule at NvtTouchCore.c:262-263):
		 * the centroid used to sum the raw delta, so the fringe of the finger's skirt kept pulling the
		 * point around and the node count swung 1..17 with pressure.  The author weights by
		 * (delta - floor) with floor = max(30 % of the blob's own peak, a noise floor), which makes the
		 * patch set pressure-invariant.  Modelled over this panel's geometry (work/tracks/
		 * touch-precision/REPORT.md §4): light-press error 16 -> 1.5 px, typical 5 -> 2.5 px, jitter
		 * 2.9 -> 1.0 px mean and 14 -> 4 px max, and coverage at the 1200 peak gate 68/101 -> 101/101.
		 *
		 * weightFloor == 0 is the SELECTION pass, which wants a plain component weight; the centroid
		 * pass passes max(3 * peak / 10, DBY_TOUCH_TOUCH_FLOOR).  The peak is taken from the UNCLAMPED
		 * delta (pass 1 no longer clamps), because a press above CEILING/0.3 would otherwise empty its
		 * own patch - the caveat the study attached to this rule. */
		w = d > DBY_TOUCH_TOUCH_CEILING ? DBY_TOUCH_TOUCH_CEILING : d;
		w = w > weightFloor ? w - weightFloor : 0u;
		count++;
		if (wsum != nullptr) {
			*wsum += w;
		}
		if (wx != nullptr) {
			*wx += (uint64_t)w * (uint64_t)c;
		}
		if (wy != nullptr) {
			*wy += (uint64_t)w * (uint64_t)r;
		}
		/* Each node is marked before it is pushed, so tail never exceeds the node count. */
		if (c + 1u < cols && delta[cur + 1u] != 0u && gTouchSeen[cur + 1u] != gen) {
			gTouchSeen[cur + 1u] = gen;
			gTouchStack[tail++] = (uint16_t)(cur + 1u);
		}
		if (c > 0u && delta[cur - 1u] != 0u && gTouchSeen[cur - 1u] != gen) {
			gTouchSeen[cur - 1u] = gen;
			gTouchStack[tail++] = (uint16_t)(cur - 1u);
		}
		if (r + 1u < rows && delta[cur + cols] != 0u && gTouchSeen[cur + cols] != gen) {
			gTouchSeen[cur + cols] = gen;
			gTouchStack[tail++] = (uint16_t)(cur + cols);
		}
		if (r > 0u && delta[cur - cols] != 0u && gTouchSeen[cur - cols] != gen) {
			gTouchSeen[cur - cols] = gen;
			gTouchStack[tail++] = (uint16_t)(cur - cols);
		}
	}
	return count;
}

/* Keep the heaviest DBY_TOUCH_MAX_CANDIDATES 4-connected components of this frame, sorted by weight, so
 * a frame with more components than slots still keeps the ones a finger could be. */
static void
dbyTouchCollectCandidates(unsigned cols, unsigned rows, unsigned nodes)
{
	const uint16_t gen = dbyTouchGenNext();

	gTouchCandCount = 0;
	for (unsigned n = 0; n < nodes; n++) {
		uint64_t w = 0;
		uint32_t wmax = 0;
		unsigned cnt, at;

		if (gTouchDelta[n] == 0u || gTouchSeen[n] == gen) {
			continue;
		}
		cnt = dbyTouchFlood(n, gTouchDelta, gen, cols, rows, &w, nullptr, nullptr, &wmax, 0u);
		/* Gate here, not only in the consumer: with 24 slots, a frame full of heavy-but-invalid
		 * components (a palm, water, a drifting reference) could otherwise evict the one real
		 * finger before it was ever considered (review should-fix d). */
		if (w == 0u || cnt < DBY_TOUCH_MIN_NODES || cnt > DBY_TOUCH_MAX_NODES ||
		    wmax < DBY_TOUCH_TOUCH_PEAK) {
			continue;
		}
		at = gTouchCandCount;
		if (at == DBY_TOUCH_MAX_CANDIDATES) {
			if (w <= gTouchCand[at - 1u].weight) {
				continue;
			}
			at--;
		} else {
			gTouchCandCount++;
		}
		while (at > 0u && gTouchCand[at - 1u].weight < w) {
			gTouchCand[at] = gTouchCand[at - 1u];
			at--;
		}
		gTouchCand[at].seed = n;
		gTouchCand[at].nodes = cnt;
		gTouchCand[at].weight = w;
		gTouchCand[at].peak = wmax;
	}
}

/* Weighted centroid of one accepted component.  The floor is a share of THIS blob's peak, capped at
 * DBY_TOUCH_TOUCH_CEILING first: the cap is what stops a hard press (raw peak >= 8000/0.3) from zeroing
 * its own patch - the "press harder and it stops registering" cliff fixed on 2026-11-03. */
static bool
dbyTouchCentroid(const struct DbyTouchCandidate *c, unsigned cols, unsigned rows,
    uint32_t *px, uint32_t *py, uint32_t *pweight, uint16_t *pnodes)
{
	const uint32_t floorPeak = c->peak > DBY_TOUCH_TOUCH_CEILING ? DBY_TOUCH_TOUCH_CEILING : c->peak;
	const uint32_t weightFloor = floorPeak * 3u / 10u > DBY_TOUCH_TOUCH_FLOOR
	    ? floorPeak * 3u / 10u : DBY_TOUCH_TOUCH_FLOOR;
	uint64_t wx = 0, wy = 0, wsum = 0;
	const unsigned count = dbyTouchFlood(c->seed, gTouchDelta, dbyTouchGenNext(), cols, rows,
	    &wsum, &wx, &wy, nullptr, weightFloor);
	uint32_t x, y;

	if (wsum == 0u || count != c->nodes) {
		return false;
	}
	/* Centroid in node units, then the half-node centre: x = (wx/wsum + 0.5) * PANEL_W / cols. */
	x = (uint32_t)(((wx * 2u + wsum) * DBY_TOUCH_PANEL_W) / (wsum * 2u * cols));
	y = (uint32_t)(((wy * 2u + wsum) * DBY_TOUCH_PANEL_H) / (wsum * 2u * rows));
	if (x >= DBY_TOUCH_PANEL_W || y >= DBY_TOUCH_PANEL_H) {
		return false;
	}
	*px = x;
	*py = y;
	*pweight = wsum > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)wsum;
	*pnodes = (uint16_t)count;
	return true;
}

/* A free id in 1..255.  The HID report packs the identifier into one byte (NativeHidInput.cpp touchSend:
 * f[1]=(unsigned char)(ids[i]&0xff)), so an id above 255 would alias a different finger. */
static uint32_t
dbyTouchNextFreeId(void)
{
	for (uint32_t candidate = 1u; candidate <= 255u; candidate++) {
		bool used = false;

		for (unsigned i = 0; i < gTouchLiveCount; i++) {
			if (gTouchContacts[i].live && gTouchContacts[i].id == candidate) {
				used = true;
				break;
			}
		}
		if (!used) {
			return candidate;
		}
	}
	return 1u;   /* at most DBY_TOUCH_MAX_CONTACTS live ids can never use 255 entries; guard only */
}

/* Every frame: deltas, components, centroids, then id tracking.  With one finger this behaves exactly
 * like the single-contact reader it replaced: one component passes the same gates, one id is published. */
static void
dbyTouchFindContact(const uint8_t *frame, size_t len)
{
	const unsigned rows = DBY_TOUCH_MATRIX_ROWS;
	const unsigned cols = DBY_TOUCH_MATRIX_COLS;
	const unsigned nodes = rows * cols;
	/* Accepted components of this frame, with their centroids.  Bounded by the contact cap: a frame
	 * cannot publish more contacts than that anyway. */
	struct DbyTouchAccepted {
		uint32_t x, y;
		uint32_t weight;
		uint16_t nodes;
	} accepted[DBY_TOUCH_MAX_CONTACTS];
	unsigned acceptedCount = 0;
	unsigned n;

	if (frame == nullptr || len < 2 + DBY_TOUCH_MATRIX_HDR + (size_t)nodes * 2) {
		goto none;
	}
	gTouchFrames++;
	if (!gTouchBaseSeeded) {
		dbyTouchSeedBase(frame);
		goto none;
	}
	/* Pass 1: every node's delta above its own downward-only reference (unchanged). */
	for (n = 0; n < nodes; n++) {
		const uint16_t raw = dbyTouchNode(frame, n);
		uint32_t d;

		if (raw < gTouchBase[n]) {
			gTouchBase[n] = raw;
		}
		d = (uint32_t)raw - (uint32_t)gTouchBase[n];
		if (d < DBY_TOUCH_TOUCH_FLOOR) {
			d = 0;
		}
		gTouchDelta[n] = (uint16_t)d;
	}

	dbyTouchCollectCandidates(cols, rows, nodes);

	/* Gates and centroids first, so the pairing below works on decided components only. */
	for (unsigned c = 0; c < gTouchCandCount && acceptedCount < DBY_TOUCH_MAX_CONTACTS; c++) {
		if (gTouchCand[c].nodes < DBY_TOUCH_MIN_NODES ||
		    gTouchCand[c].nodes > DBY_TOUCH_MAX_NODES ||
		    gTouchCand[c].peak < DBY_TOUCH_TOUCH_PEAK) {
			continue;
		}
		if (!dbyTouchCentroid(&gTouchCand[c], cols, rows, &accepted[acceptedCount].x,
		    &accepted[acceptedCount].y, &accepted[acceptedCount].weight,
		    &accepted[acceptedCount].nodes)) {
			continue;
		}
		acceptedCount++;
	}

	/* GLOBAL assignment (review 2026-11-03 18:35, must-fix 1).  Every (contact, component, distance)
	 * triple is scored, then assigned in increasing distance order, so the component closest to ANY
	 * contact wins it.  The per-contact loop this replaced processed contact 0 first: a contact that was
	 * aging out would claim a component on the far side of the panel, the finger that actually owned it
	 * got no id, and two contacts were published at the same point for three frames - the "gestures are
	 * unstable" class, on the feature this reader exists for.  No radius is involved, so a fast flick
	 * still tracks one id (a new contact starts with zero velocity and would fail any gate). */
	{
		struct DbyTouchPair {
			uint32_t dist;
			uint8_t contact;
			uint8_t candidate;
		} pairs[DBY_TOUCH_MAX_CONTACTS * DBY_TOUCH_MAX_CONTACTS];
		unsigned contactPaired[DBY_TOUCH_MAX_CONTACTS] = { 0 };
		unsigned taken[DBY_TOUCH_MAX_CONTACTS] = { 0 };
		unsigned pairCount = 0;

		for (unsigned i = 0; i < gTouchLiveCount; i++) {
			const int32_t predX = (int32_t)gTouchContacts[i].x + gTouchContacts[i].vx;
			const int32_t predY = (int32_t)gTouchContacts[i].y + gTouchContacts[i].vy;

			for (unsigned c = 0; c < acceptedCount; c++) {
				const uint32_t dx = (uint32_t)(predX > (int32_t)accepted[c].x
				    ? predX - (int32_t)accepted[c].x : (int32_t)accepted[c].x - predX);
				const uint32_t dy = (uint32_t)(predY > (int32_t)accepted[c].y
				    ? predY - (int32_t)accepted[c].y : (int32_t)accepted[c].y - predY);

				pairs[pairCount].dist = dx * dx + dy * dy;
				pairs[pairCount].contact = (uint8_t)i;
				pairs[pairCount].candidate = (uint8_t)c;
				pairCount++;
			}
		}
		/* Insertion sort: at most 10 x 10 entries, so this is a few hundred comparisons per frame. */
		for (unsigned a = 1; a < pairCount; a++) {
			const struct DbyTouchPair key = pairs[a];
			unsigned b = a;

			while (b > 0u && pairs[b - 1u].dist > key.dist) {
				pairs[b] = pairs[b - 1u];
				b--;
			}
			pairs[b] = key;
		}
		for (unsigned a = 0; a < pairCount; a++) {
			const unsigned i = pairs[a].contact;
			const unsigned c = pairs[a].candidate;

			if (contactPaired[i] || taken[c]) {
				continue;
			}
			contactPaired[i] = 1;
			taken[c] = 1;
			{
				/* Velocity for the next prediction, bounded so one bad centroid cannot fling it. */
				const int32_t stepX = (int32_t)accepted[c].x - (int32_t)gTouchContacts[i].x;
				const int32_t stepY = (int32_t)accepted[c].y - (int32_t)gTouchContacts[i].y;
				const int32_t limit = (int32_t)DBY_TOUCH_MATCH_PX;

				gTouchContacts[i].vx = stepX > limit ? limit : (stepX < -limit ? -limit : stepX);
				gTouchContacts[i].vy = stepY > limit ? limit : (stepY < -limit ? -limit : stepY);
			}
			gTouchContacts[i].x = accepted[c].x;
			gTouchContacts[i].y = accepted[c].y;
			gTouchContacts[i].weight = accepted[c].weight;
			gTouchContacts[i].nodes = accepted[c].nodes;
			gTouchContacts[i].miss = 0;
			gTouchContacts[i].live = 1;
		}

		/* Leftover components are fingers that were not down last frame: new ids, zero velocity (the
		 * recycled slot's old position must never become a velocity).  They count as paired for this
		 * frame, or the aging pass below would start them at miss = 1. */
		for (unsigned c = 0; c < acceptedCount && gTouchLiveCount < DBY_TOUCH_MAX_CONTACTS; c++) {
			unsigned slot;

			if (taken[c]) {
				continue;
			}
			slot = gTouchLiveCount++;
			gTouchContacts[slot].id = dbyTouchNextFreeId();
			gTouchContacts[slot].x = accepted[c].x;
			gTouchContacts[slot].y = accepted[c].y;
			gTouchContacts[slot].weight = accepted[c].weight;
			gTouchContacts[slot].nodes = accepted[c].nodes;
			gTouchContacts[slot].vx = 0;
			gTouchContacts[slot].vy = 0;
			gTouchContacts[slot].miss = 0;
			gTouchContacts[slot].live = 1;
			contactPaired[slot] = 1;
		}

		/* Age out the contacts that claimed nothing, then compact.  The counter is tested before it is
		 * incremented, so a contact lifts on the 4th empty frame - the same latency the single-contact
		 * reader had. */
		{
			unsigned kept = 0;

			for (unsigned i = 0; i < gTouchLiveCount; i++) {
				if (!contactPaired[i]) {
					if (gTouchContacts[i].miss < DBY_TOUCH_LIFT_DEBOUNCE) {
						gTouchContacts[i].miss++;
					} else {
						gTouchContacts[i].live = 0;
					}
				}
				if (gTouchContacts[i].live) {
					if (kept != i) {
						gTouchContacts[kept] = gTouchContacts[i];
					}
					kept++;
				}
			}
			gTouchLiveCount = kept;
		}
	}
	if (gTouchLiveCount == 0u) {
		/* Every contact lifted.  The per-contact debounce above already provided the hysteresis, so
		 * this must NOT fall into the whole-frame hold: that path returns with gTouchPixels still
		 * holding the last point, and row 42 would then print "TX c=0 x,y" with a stale coordinate
		 * for three more publishes (review 2026-11-03 18:05, must-fix 2). */
		gTouchHeld = 0;
		gTouchMissRun = 0;
		gTouchPixels[0] = DBY_TOUCH_CONTACT_NONE;
		gTouchPixels[1] = DBY_TOUCH_CONTACT_NONE;
		gTouchWeight = 0;
		gTouchNodes = 0;
		return;
	}
	/* Row 42 reports one point (the heaviest live contact) because the row is 120 bytes wide, but the
	 * dot overlay in BootProgress.c draws ONE DOT PER PUBLISHED CONTACT (it loops native_touch_count),
	 * so on the page every finger - and every phantom finger - is visible. */
	{
		unsigned heaviest = 0;

		for (unsigned i = 1; i < gTouchLiveCount; i++) {
			if (gTouchContacts[i].weight > gTouchContacts[heaviest].weight) {
				heaviest = i;
			}
		}
		gTouchPixels[0] = gTouchContacts[heaviest].x;
		gTouchPixels[1] = gTouchContacts[heaviest].y;
		gTouchWeight = gTouchContacts[heaviest].weight;
		gTouchNodes = gTouchContacts[heaviest].nodes;
	}
	if (acceptedCount != 0u) {
		gTouchHits++;   /* "confirmed contacts", not frames: a held frame must not count (SF-a) */
	}
	gTouchMissRun = 0;
	gTouchHeld = 1;
	return;
none:
	/* A frame that found nothing at all: while a contact is held its last position stands for up to
	 * DBY_TOUCH_LIFT_DEBOUNCE frames, so one weak frame cannot cut a drag in half. */
	if (gTouchHeld && gTouchMissRun < DBY_TOUCH_LIFT_DEBOUNCE) {
		gTouchMissRun++;
		return;
	}
	gTouchHeld = 0;
	gTouchMissRun = 0;
	gTouchLiveCount = 0;
	gTouchPixels[0] = DBY_TOUCH_CONTACT_NONE;
	gTouchPixels[1] = DBY_TOUCH_CONTACT_NONE;
	gTouchWeight = 0;
	gTouchNodes = 0;
}

/* Publish every live contact in the consumers' axis domain.  Odd/even sequence around the write,
 * barriers on both sides, then the sink (the upstream producer's shape, work/refs/sheng/TouchProbe.cpp:340-358).
 *
 * The axis mapping is the one verified on hardware on 2026-11-03 and is NOT repeated here in full - see
 * work/tracks/stage21-verification/REPORT.md §1.  In short: xs carries the panel's horizontal axis and ys
 * carries the vertical one, BOTH top-down since 2026-11-04 (the OS was mirrored up/down with the
 * bottom-up form; BootProgress.c mirrors the pad, which is why the overlay could not witness it).
 * 20319 rather than 20320 keeps every produced value inside the 0..20319 the consumer accepts. */
static void
dbyTouchPublishContact(void)
{
	uint32_t seq = native_touch_seq + 1;
	uint32_t count = 0;

	__atomic_store_n(&native_touch_seq, seq, __ATOMIC_RELEASE);
	__asm__ volatile("dmb ish" ::: "memory");
	for (unsigned i = 0; i < gTouchLiveCount &&
	    count < sizeof native_touch_xs / sizeof native_touch_xs[0]; i++) {
		const int x = dbyTouchClamp((int)((int64_t)gTouchContacts[i].x * DBY_TOUCH_HID_LONG_AXIS /
		    (int64_t)DBY_TOUCH_PANEL_W), DBY_TOUCH_HID_LONG_AXIS);
		const int yTopDown = dbyTouchClamp((int)((int64_t)gTouchContacts[i].y * DBY_TOUCH_HID_SHORT_AXIS /
		    (int64_t)DBY_TOUCH_PANEL_H), DBY_TOUCH_HID_SHORT_AXIS);
		/* 2026-11-04 00:12, owner on the booted OS: "下面滑动变上面滑动, 左右正常", pad overlay
		 * correct. The overlay inverts these globals again, so a wrong sign cancels out there and only
		 * the OS sees it - exactly the reported split. The HID digitizer's Y runs top-down, the panel's
		 * Y is top-down, and the consumer passes the value through unchanged: publish it top-down. */
		const int y = yTopDown;

		native_touch_xs[count] = x;
		native_touch_ys[count] = y;
		native_touch_ids[count] = (int)gTouchContacts[i].id;
		count++;
	}
	native_touch_count = count;
	__asm__ volatile("dmb ish" ::: "memory");
	__atomic_store_n(&native_touch_seq, seq + 1, __ATOMIC_RELEASE);
	gTouchPublishes++;
	if (void (*sink)(void *) = __atomic_load_n(&gTouchSink, __ATOMIC_ACQUIRE)) {
		sink(gTouchSinkContext);
	}
}

static bool
dbyTouchHidWanted(void)
{
	int want = 0;

	PE_parse_boot_argn("sheng-touch-hid", &want, sizeof want);
	return want != 0;
}

static void
stage5Thread(void *arg, wait_result_t wait)
{
	/* The vendor's get_frame() reads cd->frame_size in one transaction, and that is
	 * THP_MAX_FRAME_SIZE = 8*1024+16 = 8208 (huawei_thp.h:248, huawei_thp_core.c:2379).  A 512-byte
	 * read is a prefix of that frame, not the frame. */
	const size_t kFrameLen = 8208;
	uint8_t *buf;
	unsigned nonzero = 0;
	int rc;

	(void)arg; (void)wait;
	thread_set_thread_name(current_thread(), "dby_touch_s5");

	/* Publish at once: a frame is 65 chunked transfers and the sampling multiplies that, so this
	 * stage can run for a while.  0xFFFFFFFE means "started, not finished", which the row can show
	 * instead of the 0xFFFFFFFF "never ran". */
	gStage5Rc = 0xFFFFFFFEu;
	publishPageRow();

	buf = (uint8_t *)IOMallocAligned(kFrameLen, 64);
	if (buf == nullptr) {
		gStage5Rc = 3;
		if (gEnableRan) {
			dbyTouchResetRelease();
		}
		publishPageRow();
		return;
	}
	for (size_t i = 0; i < kFrameLen; i++) {
		buf[i] = 0;
	}

	gFailLen = 0;
	/* Stage 6 runs the vendor's enable sequence before the first frame, so the frame that follows
	 * is one a scanning chip produced rather than the idle page this driver has been reading since
	 * the identity pass.  A failure here must not stop the frame read: the row still has to show
	 * what the frame looks like, and gEnableRc says which step stopped. */
	if (gState.stage >= DBY_TOUCH_STAGE_ENABLE) {
		struct dby_touch_bus enableBus = { &gSpi, dbyBusXfer, dbyBusDelay };

		(void)dbyTouchChipEnable(&enableBus);
		publishPageRow();
	}
	{
		size_t done = 0;

		rc = stage5ReadChunked(buf, kFrameLen, &done);
		if (rc != DBY_TOUCH_OK) {
			/* Which piece failed matters more than the total length: it says whether the frame
			 * request itself was rejected (piece 0) or a later piece broke. */
			gFailLen = (uint32_t)done;
		}
	}
	if (rc != DBY_TOUCH_OK) {
		/* The snapshot was taken inside the reader, before the release transfer could overwrite it. */
		gStage5Rc = 1;
		IOFreeAligned(buf, kFrameLen);
		if (gEnableRan) {
			dbyTouchResetRelease();
		}
		publishPageRow();

		return;
	}

	gDataOff = 0xFFFFFFFFu;
	gDataRun = 0;
	gRunStart = 0xFFFFFFFFu;
	for (unsigned b = 0; b < 8; b++) {
		gBlockCount[b] = 0;
	}

	/* Everything below is measured from the first frame byte, not from the first received one. */
	const size_t kFrameStart = 2;
	/* The whole payload, not just the first 4 KiB.  The earlier 4 KiB limit came from two runs that
	 * showed everything past 4096 as "always nearly full and unchanged by touch", which was read as
	 * the capacitance image; work/tracks/n9z-web/REPORT.md now puts the matrix at payload 0x1000
	 * (8192 bytes of payload at rx[2], 2048 int16 nodes), i.e. exactly outside the old window, and
	 * notes that a per-frame counter at 1019 sits in the header.  Scanning everything is the only way
	 * to see which of the two readings the device supports. */
	const size_t kScanLen = 8192;
	{
		uint32_t run = 0;
		uint32_t run_start = 0;

		for (size_t i = kFrameStart; i < kFrameStart + kScanLen && i < kFrameLen; i++) {
			if (!dbyFrameIdle(buf[i])) {
				if (gDataOff == 0xFFFFFFFFu) {
					gDataOff = (uint32_t)(i - kFrameStart);
				}
				if (run == 0) {
					run_start = (uint32_t)(i - kFrameStart);
				}
				run++;
				nonzero++;
				/* Eight blocks over the scanned 8 KiB: 1024 bytes each.  This divided by 512 while
				 * the scan stopped at 4096, which was in range then; widening the scan to 8192 without
				 * widening the divisor would have written gBlockCount[8..15], past the end of an
				 * 8-element array.  The clamp is belt and braces: the arithmetic already cannot reach
				 * 8 because the loop stops before kScanLen. */
				if ((i - kFrameStart) / 1024u < 8u) {
					gBlockCount[(i - kFrameStart) / 1024u]++;
				}
				if (run > gDataRun) {
					gDataRun = run;
					gRunStart = run_start;
				}
			} else {
				run = 0;
			}
		}
	}
	/* The window offset is a boot argument, not a guess inside the code: one flashed build can be
	 * pointed at any offset the next run needs (dby-touch-win=<bytes>), which is how the offsets get
	 * swept without rebuilding for each candidate. */
	{
		uint32_t win = 0;
		unsigned w;

		/* Reject a value the window cannot be taken from, rather than showing an offset whose bytes
		 * were never read: the window needs 24 bytes from that point. */
		if (!PE_parse_boot_argn("dby-touch-win", &win, sizeof win) && win == 0) {
			/* No override: take the window from the longest run of real content, so one run shows
			 * the most interesting bytes without having to guess an offset first. */
			if (gRunStart != 0xFFFFFFFFu) {
				win = gRunStart;
			}
		}
		(void)gGeo;
		if (kFrameStart + (size_t)win + 24 > kFrameLen) {
			win = 0;
		}
		gWindowOff = win;
		for (w = 0; w < 6; w++) {
			const size_t o = kFrameStart + (size_t)win + w * 4;

			if (o + 4 <= kFrameLen) {
				gFrameWord[w] = ((uint32_t)buf[o] << 24) | ((uint32_t)buf[o + 1] << 16) |
				                ((uint32_t)buf[o + 2] << 8) | (uint32_t)buf[o + 3];
			} else {
				gFrameWord[w] = 0xFFFFFFFFu;
			}
		}
	}

	/* Frames across about a second, then the byte that moved the most.  This does not depend on a
	 * finger being present at one particular instant: whatever the panel reports while it is being
	 * touched will show up as a wide range somewhere in the payload.  Eight samples at 150 ms covers
	 * long enough for a human to touch the panel during the window, which four samples at 40 ms did
	 * not; the cost is roughly double the transfers, and the read loop already has a per-chunk
	 * timeout, so a dead engine fails the same way it did before. */
	{
		const unsigned kSamples = 8;
		uint8_t *minv = (uint8_t *)IOMallocAligned(kScanLen, 64);
		uint8_t *maxv = (uint8_t *)IOMallocAligned(kScanLen, 64);
		uint8_t *sample = (uint8_t *)IOMallocAligned(kFrameLen, 64);

		gLiveOff = 0xFFFFFFFFu;
		gLiveMoved = 0;
		for (unsigned b = 0; b < 8; b++) {
			gLiveBucket[b] = 0;
		}
		unsigned samples = 0;
		if (minv != nullptr && maxv != nullptr && sample != nullptr) {
			for (size_t i = 0; i < kScanLen; i++) {
				minv[i] = 0xFF;
				maxv[i] = 0x00;
			}
			for (unsigned s = 0; s < kSamples; s++) {
				size_t got = 0;

				if (stage5ReadChunked(sample, kFrameLen, &got) != DBY_TOUCH_OK) {
					break;
				}
				samples++;
				for (size_t i = 0; i < kScanLen; i++) {
					const uint8_t v = sample[kFrameStart + i];

					if (v < minv[i]) {
						minv[i] = v;
					}
					if (v > maxv[i]) {
						maxv[i] = v;
					}
				}
				IOSleep(150);
			}
			/* No successful sample leaves minv at 0xFF and maxv at 0x00; subtracting them would
			 * underflow to a huge range and publish it as if it were a measurement. */
			if (samples != 0) {
				uint32_t best = 0;

				for (size_t i = 0; i < kScanLen; i++) {
					/* Two offsets are known counters (65 toggles 0x04/0x14, 1019 increments), so
					 * they are counted as "moved" but never chosen as the widest range - otherwise an
					 * idle panel still reports the counter as the most interesting byte. */
					const uint32_t range = (maxv[i] >= minv[i])
					    ? (uint32_t)(maxv[i] - minv[i]) : 0u;
					const bool counter = (i == 65u || i == 1019u);

					if (range >= 8) {
						const size_t bucket = i / 1024u;

						gLiveMoved++;
						if (bucket < 8u && gLiveBucket[bucket] < 0xFFu) {
							gLiveBucket[bucket]++;
						}
					}
					if (!counter && range > best) {
						best = range;
						gLiveOff = (uint32_t)i;
						gLiveMin = minv[i];
						gLiveMax = maxv[i];
					}
				}
			}
		}
		if (minv != nullptr) {
			IOFreeAligned(minv, kScanLen);
		}
		if (maxv != nullptr) {
			IOFreeAligned(maxv, kScanLen);
		}
		if (sample != nullptr) {
			IOFreeAligned(sample, kFrameLen);
		}
	}
	/* The geometry read latches a different page, and this chip's 0x20 read returns whatever page is
	 * current - so it runs only after the frames have been taken.  Reading it first changed the frame
	 * from the sparse report page (267 real bytes) into the dense matrix page (4029). */
	{
		int want = 0;

		PE_parse_boot_argn("dby-touch-dump", &want, sizeof want);
		/* 2026-11-03: skip the sweep when the live HID loop is going to run anyway.  The sweep is
		 * 40 frames 2 s apart, i.e. it delays the first usable touch by ~82 s after boot, and it holds
		 * this thread the whole time.  With sheng-touch-hid=1 the live loop below republishes every
		 * 25 ms, which is what the UI actually consumes, so the sweep only buys offline frames at the
		 * cost of a minute and a half of dead touch.  Offline evidence is still available by booting
		 * without sheng-touch-hid=1. */
		if (want && !dbyTouchHidWanted()) {
			/* Forty frames, two seconds apart: an 80-second window inside ONE boot.  Four frames
			 * were not enough - a press that starts after the window, or one held since boot and
			 * already absorbed by the firmware's baseline tracking, both look like "no contact".
			 * With 80 seconds the owner can press and release each corner and the sweep catches
			 * every edge wherever it falls. */
			dbyDumpFrame(buf, kFrameLen, 0);
			dbyTouchFindContact(buf, kFrameLen);
			for (unsigned i = 1; i < DBY_TOUCH_FRAME_SNAPSHOTS; i++) {
				size_t again = 0;

				IOSleep(DBY_TOUCH_FRAME_PERIOD_MS);
				if (stage5ReadChunked(buf, kFrameLen, &again) == DBY_TOUCH_OK) {
					dbyDumpFrame(buf, kFrameLen, i);
					dbyTouchFindContact(buf, kFrameLen);
				} else {
					break;
				}
			}
		}
	}
	/* Live publication.  The bounded sweep above records evidence; a UI needs a stream, so with
	 * `sheng-touch-hid=1` this thread keeps sampling and publishes every frame.
	 *
	 * The loop cannot move below the FWINFO read to let that read run: `stage5ReadChunked` sends 0x20
	 * with no page latch, so it returns whatever page the chip has selected, and FWINFO selects a
	 * different page on purpose (see the comment on that block).  So everything this thread would
	 * normally do afterwards is done first - the counters below - and the row is refreshed from
	 * inside the loop, because nothing else in this thread will run again. */
	gFrameNz = nonzero;
	gStage5Rc = 0;
	if (dbyTouchHidWanted()) {
		unsigned fails = 0;

		for (;;) {
			size_t again = 0;

			IOSleep(DBY_TOUCH_HID_PERIOD_MS);
			if (stage5ReadChunked(buf, kFrameLen, &again) == DBY_TOUCH_OK) {
				fails = 0;
				dbyTouchFindContact(buf, kFrameLen);
				dbyTouchPublishContact();
				/* Refresh the page every 16 publishes: ~0.6 s at the 2026-11-03 sample period
				 * (25 ms sleep + ~15 ms frame read), which is inside the log writer's 2 s cadence. */
				if ((gTouchPublishes & 15u) == 0u) {
					publishPageRow();
				}
			} else {
				/* A failed read is not a lift, so the last contact stands.  Back off so a dead
				 * engine costs one attempt per period instead of a full-rate busy retry: a
				 * failed transfer can spend up to ~200 ms in IODelay (GeniSpi.c:199-208). */
				gTouchReadFails++;
				fails++;
				IOSleep(DBY_TOUCH_HID_PERIOD_MS * (fails > 8u ? 8u : fails));
			}
		}
	}
	/* FWINFO: the only place this generation publishes its matrix geometry.
	 *
	 * Address chain, from the vendor tree and the public Novatek drivers: the event buffer is
	 * 0x2FE00 (three independent alignments: N9Z_CMD_ADDR 0x2FE50 minus HOST_CMD +0x50,
	 * N9Z_CMD_STAMP 0x2FE77 minus +0x77, and the FWINFO block at +0x78).  The read is a page select
	 * followed by a 40-byte transfer whose first two bytes are the command phase, so the block starts
	 * at rx[2]; the vendor calls rx[2] "the valid value" for exactly this reason.
	 * rx[2]+rx[3]==0xFF identifies a real answer, x_num and y_num follow at rx[4] and rx[5]. */
	{
		struct dby_touch_bus bus = { &gSpi, dbyBusXfer, dbyBusDelay };
		uint8_t latch[3] = { 0xFF, 0x05, 0xFC };   /* 0x2FE78 >> 15, (0x2FE78 >> 7) & 0xFF */
		/* 40 bytes, not 19: work/tracks/nvt-header/SPEC.md:V1 reads the block from 0x78 for 38
		 * bytes, and the extra tail costs nothing at a length the engine has already been driven
		 * to.  The block's first four bytes are the check pair and the two dimensions. */
		uint8_t txr[40] = { 0x78, 0x00 };
		uint8_t rx[40] = { 0 };

		if (bus.xfer(bus.ctx, latch, NULL, 3) == DBY_TOUCH_OK &&
		    bus.xfer(bus.ctx, txr, rx, sizeof txr) == DBY_TOUCH_OK) {
			for (unsigned i = 0; i < 8; i++) {
				gFwByte[i] = rx[i];
			}
			/* The vendor's own validity test for this block: the two check bytes at b[0]/b[1]
			 * (payload rx[2]/rx[3]) add up to 0xFF. */
			gFwValid = ((unsigned)rx[2] + (unsigned)rx[3] == 0xFFu) ? 1u : 0u;
			/* b[2] and b[3] are x_num and y_num, i.e. rx[4] and rx[5]: the node count is
			 * 2*x_num*y_num bytes, which is what decides 40x60 against 40x64 (and rules both out
			 * if the matrix is only the payload's last 4096 bytes). */
			gFwX = rx[4];
			gFwY = rx[5];
		}
	}
	gFrameNz = nonzero;
	gStage5Rc = 0;
	/* Leave the panel in the only state the rest of the boot can use: out of reset.  The enable
	 * sequence already did this, but the frame read and the dump ran after it, so re-assert it
	 * once here rather than trusting that nothing disturbed the line. */
	if (gEnableRan) {
		dbyTouchResetRelease();
	}
	IOFreeAligned(buf, kFrameLen);
	publishPageRow();
	IOLog("DbyTouch(stage5): nz=%u off=%u run=%u rs=%u win=%u b=%u,%u,%u,%u,%u,%u,%u,%u w=%08x %08x %08x %08x\n",
	    nonzero, gDataOff, gDataRun, gRunStart, gWindowOff,
	    gBlockCount[0], gBlockCount[1], gBlockCount[2], gBlockCount[3],
	    gBlockCount[4], gBlockCount[5], gBlockCount[6], gBlockCount[7],
	    gFrameWord[0], gFrameWord[1], gFrameWord[2], gFrameWord[3]);
}

/* ------------------------------------------------------------------ */
/* SPMI PMIC arbiter: reboot reason                                    */
/* ------------------------------------------------------------------ */

/* The next boot goes where the stock bootloader is told to go by one byte of PMIC scratch memory,
 * so a PSCI reset only lands in fastboot if that cell says "bootloader" first.  This is the same
 * mechanism UniXNU-sheng uses (package/BootResetCore.h); the addresses below are kona's, taken from
 * sm8250.dtsi `spmi@c440000`:
 *   core 0x0C440000 (0x1100), chnls 0x0C600000 (0x2000000),
 *   obsrvr 0x0E600000 (0x100000), cnfg 0x0C40A000 (0x26000).
 * sheng is SM8550 and uses a completely different set (0xc400000/0xc500000/0xc440000/0xc42d000);
 * copying those here would drive unrelated registers.
 *
 * Returns 0 when the cell was written and read back; other values name the failing stage. */

/* Stage 2 (official ladder step 2): vote the QUPv3 wrapper-2 clocks and wait for the branches, then
 * configure the pins.  This runs on its own thread because the entry point is called from
 * boot_progress_poll(), which NativeUfs can invoke while holding its window lock; voting and
 * waiting must not sleep there.
 *
 * Registers come from work/refs/kona-qupv3-se14-clocks.md (transcribed from mainline
 * drivers/clk/qcom/gcc-sm8250.c).  They are NOT the SM8550 values sheng's TouchBoard.h carries:
 * on kona the QUP vote register is 0x52010 (sheng 0x52008) and wrapper 2's halts are
 * 0x1e004/0x1e008/0x1e00c/0x23270/0x23278.
 *
 * A branch with the hardware-clock-gating bit (CBCR bit 1) may show CLK_OFF while idle and still be
 * healthy; a branch without it must have CLK_OFF clear (same rule as sheng TouchProbe.cpp:399-406).
 */
static void
stage2Thread(void *arg, wait_result_t wait)
{
	DbyTouchMap *gccMap = &gState.gcc;
	unsigned tries;

	(void)arg; (void)wait;
	thread_set_thread_name(current_thread(), "dby_touch_s2");

	if (!mapPhys(DBY_TOUCH_GCC_BASE, DBY_TOUCH_GCC_SIZE, gccMap)) {
		gClkOk = 0;
		gClkTries = DBY_TOUCH_PROBE_FAIL;
		publishPageRow();
		return;
	}

	/* GPLL0 feeds the QUP DFS levels: a shared vote, not an exclusive enable.  wr32Checked is this
	 * driver's write idiom (store, dsb, read back); the branches below are what prove the vote took
	 * effect, so the PLL's own lock bit is not polled here. */
	{
		/* Keep every other bit of both registers: a read that comes back 0xFFFFFFFF (an unclocked
		 * GCC responds like that) must not be OR-ed into a write. */
		uint32_t gpll0 = rd32(gccMap->base, DBY_TOUCH_GCC_GPLL0_ENABLE);

		if (gpll0 != 0xFFFFFFFFu) {
			(void)wr32Checked(gccMap->base, DBY_TOUCH_GCC_GPLL0_ENABLE,
			    (gpll0 & ~0x1U) | 0x1U, 0x1U);
		}
	}
	{
		uint32_t vote = rd32(gccMap->base, DBY_TOUCH_GCC_VOTE);

		if (vote != 0xFFFFFFFFu) {
			(void)wr32Checked(gccMap->base, DBY_TOUCH_GCC_VOTE,
			    (vote & ~DBY_TOUCH_VOTE_WRAP2_ALL) | DBY_TOUCH_VOTE_WRAP2_ALL,
			    DBY_TOUCH_VOTE_WRAP2_ALL);
		}
	}

	for (tries = 0; tries < 50; tries++) {
		uint32_t m, s, s0, c, c2;

		m  = rd32(gccMap->base, DBY_TOUCH_GCC_HALT_M_AHB);
		s  = rd32(gccMap->base, DBY_TOUCH_GCC_HALT_S_AHB);
		s0 = rd32(gccMap->base, DBY_TOUCH_GCC_HALT_S0);
		c  = rd32(gccMap->base, DBY_TOUCH_GCC_HALT_CORE);
		c2 = rd32(gccMap->base, DBY_TOUCH_GCC_HALT_CORE_2X);
		gClkHaltMAhb = m; gClkHaltSAhb = s; gClkHaltCore = c; gClkHaltS0 = s0; gClkHaltCore2x = c2;
		gClkVotePost = rd32(gccMap->base, DBY_TOUCH_GCC_VOTE);
		gClkOk = ((!(m & DBY_TOUCH_CBCR_CLK_OFF)  || (m & DBY_TOUCH_CBCR_HWCG)) &&
		          (!(s & DBY_TOUCH_CBCR_CLK_OFF)  || (s & DBY_TOUCH_CBCR_HWCG)) &&
		          (!(s0 & DBY_TOUCH_CBCR_CLK_OFF) || (s0 & DBY_TOUCH_CBCR_HWCG)) &&
		          (!(c & DBY_TOUCH_CBCR_CLK_OFF)  || (c & DBY_TOUCH_CBCR_HWCG)) &&
		          (!(c2 & DBY_TOUCH_CBCR_CLK_OFF) || (c2 & DBY_TOUCH_CBCR_HWCG))) ? 1U : 0U;
		if (gClkOk) {
			break;
		}
		IOSleep(1);
	}
	gClkTries = tries;
	gStage2Done = true;
	publishPageRow();

	/* The SE's own root clock, before anything is read from the engine.  Voting a branch does not
	 * start its parent RCG, and a stopped parent still reports CLK_OFF clear, so the branch poll
	 * above cannot distinguish "clocked" from "parent dead".  Order matters: root-enable, configure,
	 * update, then release the force-enable (see DbyTouchBoard.h). */
	{
		uint32_t rcgCmd = rd32(gccMap->base, DBY_TOUCH_GCC_SE_RCG_CMD);
		uint32_t rcgCfg = rd32(gccMap->base, DBY_TOUCH_GCC_SE_RCG_CFG);
		boolean_t armed = FALSE;
		unsigned n;

		/* A GCC that answers 0xFFFFFFFF must not be read-modify-written back: the OR would set
		 * UPDATE and every reserved bit at once. */
		if (rcgCmd != 0xFFFFFFFFu && rcgCfg != 0xFFFFFFFFu) {
			armed = wr32Checked(gccMap->base, DBY_TOUCH_GCC_SE_RCG_CMD,
			    rcgCmd | DBY_TOUCH_GCC_RCG_ROOT_EN, DBY_TOUCH_GCC_RCG_ROOT_EN);
			if (armed) {
				/* Only the documented fields, so a read-only or reserved bit cannot fail the
				 * compare; this read-back is what proves the divider was latched. */
				(void)wr32Checked(gccMap->base, DBY_TOUCH_GCC_SE_RCG_CFG,
				    DBY_TOUCH_GCC_SE_RCG_CXO_19M, DBY_TOUCH_GCC_RCG_CFG_MASK);
				/* CMD_UPDATE is write-1 and self-clears, so its read-back must not be asserted:
				 * the update can finish before the read returns and the compare would always fail,
				 * latching "never wrote" and disabling the engine for good. */
				(void)wr32Checked(gccMap->base, DBY_TOUCH_GCC_SE_RCG_CMD,
				    rcgCmd | DBY_TOUCH_GCC_RCG_ROOT_EN | DBY_TOUCH_GCC_RCG_UPDATE,
				    DBY_TOUCH_GCC_RCG_UPDATE);

				/* While ROOT_EN still holds the root, ROOT_OFF clearing is the proof that this
				 * root actually came up (mainline's clk_rcg2_set_force_enable waits the same way).
				 * Checking it *after* the release would only measure the child branches. */
				for (n = 0; n < 500; n++) {
					uint32_t cmd = rd32(gccMap->base, DBY_TOUCH_GCC_SE_RCG_CMD);

					if ((cmd & DBY_TOUCH_GCC_RCG_ROOT_OFF) == 0 &&
					    (cmd & DBY_TOUCH_GCC_RCG_UPDATE) == 0) {
						gSeRcgOk = 1;
						break;
					}
					IOSleep(1);
				}
				/* Release the force-enable whatever happened: leaving ROOT_EN set pins the root on
				 * and confuses the clock framework for every later user of it. */
				(void)wr32Checked(gccMap->base, DBY_TOUCH_GCC_SE_RCG_CMD,
				    rd32(gccMap->base, DBY_TOUCH_GCC_SE_RCG_CMD) &
				    ~DBY_TOUCH_GCC_RCG_ROOT_EN,
				    DBY_TOUCH_GCC_RCG_ROOT_EN);
			}
		}
		gSeRcgCmd = rd32(gccMap->base, DBY_TOUCH_GCC_SE_RCG_CMD);
		(void)rcgCfg;
	}

	/* Decisive experiment (reviews f54a146e item 5 and 7a9cdb75): is the vote register 0x52010 wired
	 * to this branch at all?
	 *
	 * The vote is a level and it already read 0x1f, so re-writing it cannot change anything.  What
	 * answers the question is to drop this branch's vote bit and watch gcc_qupv3_wrap2_s0_clk's
	 * halted-status register at 0x1e00c:
	 *
	 *   s0b != s0a                     -> the wiring exists; the engine's silence is elsewhere
	 *   s0b == s0a and the clear landed -> bit4 does not drive 0x1e00c; more votes are pointless
	 *   s0b == s0a and the clear failed  -> no conclusion, the experiment did not run
	 *
	 * The clear's own read-back (gVoteCleared) is published for exactly that reason: without it the
	 * last two outcomes look identical on the page. */
	{
		uint32_t vote = rd32(gccMap->base, DBY_TOUCH_GCC_VOTE);

		if (vote != 0xFFFFFFFFu) {
			gVoteBefore = vote;
			gS0Before = rd32(gccMap->base, DBY_TOUCH_GCC_HALT_S0);
			(void)wr32Checked(gccMap->base, DBY_TOUCH_GCC_VOTE,
			    vote & ~DBY_TOUCH_VOTE_WRAP2_S0, DBY_TOUCH_VOTE_WRAP2_S0);
			gVoteCleared = rd32(gccMap->base, DBY_TOUCH_GCC_VOTE);
			gS0NoVote = rd32(gccMap->base, DBY_TOUCH_GCC_HALT_S0);
			/* Restore from a fresh read so a bit changed by anyone else during the window is not
			 * rolled back; the 0xFFFFFFFF guard stays, otherwise the OR would write it straight back. */
			{
				uint32_t now = rd32(gccMap->base, DBY_TOUCH_GCC_VOTE);

				if (now != 0xFFFFFFFFu) {
					(void)wr32Checked(gccMap->base, DBY_TOUCH_GCC_VOTE,
					    now | DBY_TOUCH_VOTE_WRAP2_S0, DBY_TOUCH_VOTE_WRAP2_S0);
				}
			}
			gS0ReVote = rd32(gccMap->base, DBY_TOUCH_GCC_HALT_S0);
			gClkVotePost = rd32(gccMap->base, DBY_TOUCH_GCC_VOTE);
			gClkHaltS0 = gS0ReVote;
			gClkHaltCore = rd32(gccMap->base, DBY_TOUCH_GCC_HALT_CORE);
			gClkHaltCore2x = rd32(gccMap->base, DBY_TOUCH_GCC_HALT_CORE_2X);
		}
	}

	/* Pins next: the pin tiles are already mapped by the entry point. */
	applyPinConfig();
	publishPageRow();

	/* The engine itself stays untouched unless explicitly asked: an unclocked or half-voted SE read
	 * is what stalled the interconnect on 2026-10-31. */
	{
		int want_se = 0;

		PE_parse_boot_argn("dby-touch-se", &want_se, sizeof want_se);
		if (want_se && gClkOk && gSeRcgOk) {
			/* Write/read-back first, with the value the official geni_spi_init() writes anyway
			 * (GeniSpi.c:86-87): 0x7f into GENI_OUTPUT_CTRL and bit0 into GENI_FORCE_DEFAULT.
			 * This separates "the SE window is dead" from "the SE is simply disabled", which the
			 * identity reads alone cannot tell apart - they return zero in both cases. */
			{
				DbyTouchMap *seMap = &gState.se;

				if (seMap->base != 0) {
					(void)wr32Checked(seMap->base, DBY_TOUCH_SE_GENI_OUTPUT_CTRL,
					    DBY_TOUCH_GENI_DEFAULT_OUTPUT_CTRL,
					    DBY_TOUCH_GENI_DEFAULT_OUTPUT_CTRL);
					(void)wr32Checked(seMap->base, DBY_TOUCH_SE_GENI_FORCE_DEFAULT,
					    DBY_TOUCH_GENI_FORCE_DEFAULT_BIT,
					    DBY_TOUCH_GENI_FORCE_DEFAULT_BIT);
					gSeWriteBack = rd32(seMap->base, DBY_TOUCH_SE_GENI_OUTPUT_CTRL);
					/* HW_PARAM_0 is a silicon constant: [29:24] tx width, [21:16] tx FIFO depth.
					 * The vendor driver prints 16 / 32 for this very SE, so 0x20..0x10.. is the
					 * expected shape and a zero read means the engine still has no clock. */
					gSeHwParam0 = rd32(seMap->base, DBY_TOUCH_SE_HW_PARAM_0);
					/* Two words that decide the next step.
					 * wrapper+0x0: the vendor tree gives it no name, but mainline calls the same
					 *   offset QUP_SW_RESET with one bit per SE; if our SE's bit is set the engine
					 *   block is held in reset and every access inside it reads zero - which is
					 *   exactly what wb=00000000 showed.
					 * se+0x0: GENI_INIT_CFG_REVISION.  It may legitimately read zero, so it is
					 *   evidence only when non-zero; the proof of a live engine is HW_PARAM_0. */
					if (gState.qup.base != 0) {
						gWrapWord0 = rd32(gState.qup.base, 0x0U);
					}
					/* An all-ones OUTPUT_CTRL read-back means the window is not answering (no clock,
					 * or an undecoded address); stop reading rather than walk a dead window. */
					if (gSeWriteBack != 0xFFFFFFFFu) {
						gSeWord0 = rd32(seMap->base, 0x0U);
					}
				}
			}
			seCensus();
			publishPageRow();
			/* Only once the engine itself answered: a transfer against a silent engine would be
			 * the 2026-10-31 mistake with extra steps. */
			if (gState.seVerdict == kSeVerdictFifoUsable) {
				thread_t stage3;

				if (kernel_thread_start(stage3Thread, NULL, &stage3) == KERN_SUCCESS) {
					thread_deallocate(stage3);
				}
			} else {
				gStage3Rc = 0xFFFFFFFEu;   /* not attempted: the engine verdict says no */
			}
		}
	}
	IOLog("DbyTouch(stage2): vote=%08x ok=%u tries=%u\n", gClkVotePost, gClkOk, gClkTries);
}

extern "C" int
dby_spmi_set_bootloader(char *out, unsigned capacity, uint8_t *before, uint8_t *after,
    unsigned *detail)
{
	/* Register geometry of the SPMI PMIC arbiter, per mainline drivers/spmi/spmi-pmic-arb.c:
	 *   v5 (0x50000000..0x70000000): APID map at core+0x900, APID owner at cnfg+0x700+4n,
	 *                                observer 0x10000*ee + 0x80*apid, read/write 0x10000*apid
	 *   v7 (0x70000000..0x80000000): APID map at core+0x2000, APID owner at cnfg+4n,
	 *                                observer 0x8000*ee + 0x20*apid, read/write 0x1000*apid
	 * The first attempt assumed v7 and this board answered rc=2 "spmi:layout" on 2026-11-01, so
	 * the layout is now selected from the version register instead of assumed. */
	enum {
		kCore = 0, kCnfg, kObs, kChnl,
		kVersion = 0x00, kFeatures = 0x04, kFeaturesMask = 0x7ffu,
		kCmd = 0x00, kStatus = 0x08, kWdata0 = 0x10, kRdata0 = 0x18,
		kOpExtWriteL = 0, kOpExtReadL = 1,
		kDone = 1u << 0, kErrors = 0xeu,
		kSdamPpid = 0x071,
		kRebootCell = 0x48, kRebootMask = 0xfe, kRebootShift = 1, kRebootBootloader = 2,
		kPolls = 200000
	};
	static const uint64_t kBase[4] = { 0x0C440000ULL, 0x0C40A000ULL, 0x0E600000ULL, 0x0C600000ULL };
	static const uint64_t kSize[4] = { 0x00004000ULL, 0x00026000ULL, 0x00100000ULL, 0x02000000ULL };
	DbyTouchMap win[4] = {};
	volatile uint32_t *core;
	volatile uint32_t *cnfg;
	uint32_t version;
	uint32_t apidMapOff, ownerOff, obsStride, apidStride, obsEeStride;
	uint32_t mark = 0;             /* the version tag, for the caller's text */
	int rc = 0;
	int apid = -1;
	unsigned k;

	if (detail != nullptr) {
		*detail = 0;
	}
	for (k = 0; k < 4; k++) {
		if (!mapPhys(kBase[k], kSize[k], &win[k])) {
			rc = 1;
			goto out;
		}
	}
	core = reinterpret_cast<volatile uint32_t *>(win[kCore].base);
	cnfg = reinterpret_cast<volatile uint32_t *>(win[kCnfg].base);
	version = core[kVersion / 4];

	if (version >= 0x50000000u && version < 0x70000000u) {
		mark = 5;
		apidMapOff = 0x900;
		ownerOff = 0x700;
		obsStride = 0x80;
		obsEeStride = 0x10000;
		apidStride = 0x10000;
	} else if (version >= 0x70000000u && version < 0x80000000u) {
		mark = 7;
		apidMapOff = 0x2000;
		ownerOff = 0x000;
		obsStride = 0x20;
		obsEeStride = 0x8000;
		apidStride = 0x1000;
	} else {
		rc = 2;                  /* a generation this code does not know */
		goto out;
	}
	{
		uint32_t apids = core[kFeatures / 4] & kFeaturesMask;
		unsigned i;

		dby_spmi_version = (version >> 24) & 0xffu;

		if (apids == 0) {
			rc = 3;
			goto out;
		}
		/* mainline refuses to walk past the mapped core window (spmi-pmic-arb.c:1285).  The v7 APID
		 * map starts at core+0x2000 and the window is 0x4000, so 512 entries is the most this code can
		 * read; anything larger would read undecoded addresses and abort. */
		if (apidMapOff + 4u * apids > (uint32_t)kSize[kCore]) {
			apids = (uint32_t)(kSize[kCore] - apidMapOff) / 4u;
		}
		if (apids > 512u) {
			apids = 512u;
		}
		dby_spmi_apids = apids;
		gSpmiVersion = version;
		gSpmiApidCount = apids;
		gSpmiPpidCount = 0;
		for (i = 0; i < apids; i++) {
			uint32_t entry = core[(apidMapOff + 4 * i) / 4];
			uint32_t ppid = (entry >> 8) & 0xfffu;
			int owner;

			/* Collect the distinct PPIDs actually present, for the page. */
			if (ppid != 0 && gSpmiPpidCount < 6) {
				unsigned q;
				bool seen = false;

				for (q = 0; q < gSpmiPpidCount; q++) {
					if (gSpmiPpid[q] == ppid) {
						seen = true;
					}
				}
				if (!seen) {
					gSpmiPpid[gSpmiPpidCount++] = ppid;
				}
			}
			if (entry == 0 || ppid != kSdamPpid) {
				continue;
			}
			owner = static_cast<int>(cnfg[(ownerOff + 4 * i) / 4] & 7u);
			/* mainline rejects a write whose APID this EE does not own (-EPERM), so only an APID
			 * owned by EE 0 is acceptable here. */
			if (owner == 0) {
				apid = static_cast<int>(i);
				break;
			}
		}
		if (apid < 0) {
			rc = 4;
			goto out;
		}
	}
	{
		volatile uint32_t *obsWin = reinterpret_cast<volatile uint32_t *>(win[kObs].base);
		volatile uint32_t *chnWin = reinterpret_cast<volatile uint32_t *>(win[kChnl].base);
		volatile uint32_t *obs;
		volatile uint32_t *chn;
		uint8_t beforeVal = 0xFF, afterVal = 0xFF, next;
		unsigned n;

		if (obsEeStride * 0u + obsStride * static_cast<uint32_t>(apid) > kSize[kObs] - 0x20) {
			rc = 12;
			goto out;
		}
		if (apidStride * static_cast<uint32_t>(apid) > kSize[kChnl] - 0x20) {
			rc = 13;
			goto out;
		}
		obs = obsWin + (obsStride * static_cast<uint32_t>(apid)) / 4;   /* EE 0 */
		chn = chnWin + (apidStride * static_cast<uint32_t>(apid)) / 4;

		auto cmd = [](uint8_t op, uint8_t addr) {
			return (static_cast<uint32_t>(op) << 27) | (static_cast<uint32_t>(addr) << 4);
		};
		auto wait = [](volatile uint32_t *ch) -> int {
			for (unsigned p = 0; p < kPolls; p++) {
				uint32_t st = ch[kStatus / 4];

				if (st & kDone) {
					return (st & kErrors) ? 2 : 0;
				}
			}
			return 1;
		};

		obs[kCmd / 4] = cmd(kOpExtReadL, kRebootCell);
		n = static_cast<unsigned>(wait(obs));
		if (n != 0) {
			rc = 4 + static_cast<int>(n);
			goto out;
		}
		beforeVal = static_cast<uint8_t>(obs[kRdata0 / 4]);

		next = static_cast<uint8_t>((beforeVal & ~kRebootMask) | (kRebootBootloader << kRebootShift));
		chn[kWdata0 / 4] = next;
		chn[kCmd / 4] = cmd(kOpExtWriteL, kRebootCell);
		n = static_cast<unsigned>(wait(chn));
		if (n != 0) {
			rc = 6 + static_cast<int>(n);
			goto out;
		}
		obs[kCmd / 4] = cmd(kOpExtReadL, kRebootCell);
		n = static_cast<unsigned>(wait(obs));
		if (n != 0) {
			rc = 8 + static_cast<int>(n);
			goto out;
		}
		afterVal = static_cast<uint8_t>(obs[kRdata0 / 4]);
		if (before != nullptr) {
			*before = beforeVal;
		}
		if (after != nullptr) {
			*after = afterVal;
		}
		if (afterVal != next) {
			rc = 11;
		}
	}

out:
	for (k = 0; k < 4; k++) {
		unmapWindow(&win[k]);
	}
	if (detail != nullptr) {
		/* The generation matters more than the code when the text is shown: it says which layout
		 * ran, and BootResetDby.c overwrites the text on success. */
		*detail = (mark << 8) | static_cast<unsigned>(rc & 0xffu);
	}
	if (out != nullptr && capacity != 0) {
		unsigned at = 0;
		const char *p = (rc == 0) ? "spmi ok v" : "spmi rc";

		while (*p != '\0' && at + 1 < capacity) {
			out[at++] = *p++;
		}
		/* rc and the detected generation, so one read of the page says which layout ran. */
		if (at + 1 < capacity) {
			out[at++] = static_cast<char>((rc < 0) ? '-' : ('0' + (rc % 10)));
		}
		if (at + 1 < capacity) {
			out[at++] = 'v';
			out[at++] = static_cast<char>('0' + (mark % 10));
		}
		out[at] = '\0';
	}
	return rc;
}

/* ------------------------------------------------------------------ */
/* Physical buttons: PM8150 PON (power, volume down) + GPIO6 (volume up) */
/* ------------------------------------------------------------------ */
/*
 * All three keys sit on the PM8150, so this needs no pin controller of its own: the SPMI
 * PMIC arbiter the touch driver already speaks to is enough.  Every address below has a
 * line in this board's own kernel, none of it is inferred:
 *
 *   pm8150.dtsi:16,20,70      PM8150 is SPMI sid 0; power-on at 0x800; pinctrl at 0xc000
 *   kona-qrd.dtsi:352-357     vol_up = <&pm8150_gpios 6 GPIO_ACTIVE_LOW>
 *   qpnp-power-on.c:122-123   QPNP_PON_KPDPWR_N_SET = bit0, QPNP_PON_RESIN_N_SET = bit1
 *   qpnp-power-on.c:282       "Triggered from RESIN_N (Reset Line/Volume Down Key)"
 *   qpnp-power-on.c:1021      input_report_key(..., key_status) => a set RT_STS bit means pressed
 *   pinctrl-spmi-gpio.c:38    RT_STS 0x10, bit0 is the pad level; 0x40 MODE, 0x41 VIN, 0x42 PULL
 *   qcom,pmic-gpio.h:9        PMIC_GPIO_PULL_UP_30 = 0
 *   NativeHidInput.cpp:305    the HID device registers its sink through native_buttons_set_sink()
 *
 * Publishing is report 2: bit1 Power, bit2 Volume Increment, bit3 Volume Decrement
 * (NativeHidInput.cpp:22-23), which is the same encoding buttonsNotify() expects.
 *
 * The gate is sheng-buttons=1, deliberately separate from dby-touch= so the keys keep
 * working on a boot that does not start the touch ladder.
 */

typedef void (*dbyButtonSink)(void *context, uint32_t mask);

enum {
	kBtnReportPower   = 1u << 1,
	kBtnReportVolUp   = 1u << 2,
	kBtnReportVolDown = 1u << 3,

	kPmicPpidPon   = 0x0008,        /* sid 0, peripheral 0x0800 */
	kPmicPpidGpio6 = 0x00C5,        /* sid 0, peripheral 0xC500 */

	kPmicRegRtSts   = 0x10,
	kPmicRegModeCtl = 0x40,
	kPmicRegVinCtl  = 0x41,
	kPmicRegPullCtl = 0x42,

	kPmicCore = 0, kPmicObs = 2, kPmicChn = 3,
	kPmicCmd = 0x00, kPmicStatus = 0x08, kPmicWdata0 = 0x10, kPmicRdata0 = 0x18,
	kPmicOpWrite = 0, kPmicOpRead = 1,
	kPmicDone = 1u << 0, kPmicErrors = 0xeu,

	kBtnPollMs = 10,
	kBtnStableFrames = 3
};

struct dbyPmicBus {
	bool tried;
	bool ready;
	uint32_t mark;                  /* arbiter generation, 5 or 7 */
	DbyTouchMap win[4];
	uint32_t obsStride;
	uint32_t apidStride;
	uint32_t apidMapOff;
	uint32_t apidCount;
	int apidPon;
	int apidGpio6;
	uint32_t lastPon;
	uint32_t lastGpio;
	uint32_t errors;
};

static dbyPmicBus gPmic;
static dbyButtonSink gBtnSink;
static void *gBtnSinkCtx;
static uint32_t gBtnMask;
static uint32_t gBtnSinkCalls;
static bool gBtnStarted;

static volatile uint32_t *
dbyPmicChannel(unsigned which, int apid)
{
	uint32_t stride = (which == kPmicObs) ? gPmic.obsStride : gPmic.apidStride;

	return reinterpret_cast<volatile uint32_t *>(gPmic.win[which].base) +
	    (stride * static_cast<uint32_t>(apid)) / 4;
}

static bool
dbyPmicWait(volatile uint32_t *channel)
{
	for (unsigned poll = 0; poll < 200000u; poll++) {
		uint32_t status = channel[kPmicStatus / 4];

		if (status & kPmicDone) {
			return (status & kPmicErrors) == 0;
		}
	}
	return false;
}

/* PPID -> APID.  The arbiter's map entry carries the ppid in bits 19:8, and only an APID
 * that appears there can be addressed at all (spmi-pmic-arb.c). */
static int
dbyPmicApid(uint32_t ppid)
{
	volatile uint32_t *core;

	if (gPmic.apidCount == 0) {
		return -1;
	}
	core = reinterpret_cast<volatile uint32_t *>(gPmic.win[kPmicCore].base);
	for (uint32_t i = 0; i < gPmic.apidCount; i++) {
		uint32_t entry = core[(gPmic.apidMapOff + 4u * i) / 4];

		if (((entry >> 8) & 0xfffu) == ppid) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

static bool
dbyPmicRead(int apid, uint8_t address, uint8_t *out)
{
	volatile uint32_t *observer;

	if (apid < 0 || out == nullptr) {
		return false;
	}
	observer = dbyPmicChannel(kPmicObs, apid);
	observer[kPmicCmd / 4] = (static_cast<uint32_t>(kPmicOpRead) << 27) |
	    (static_cast<uint32_t>(address) << 4);
	if (!dbyPmicWait(observer)) {
		gPmic.errors++;
		return false;
	}
	*out = static_cast<uint8_t>(observer[kPmicRdata0 / 4] & 0xffu);
	return true;
}

static bool
dbyPmicWrite(int apid, uint8_t address, uint8_t value)
{
	volatile uint32_t *command;

	if (apid < 0) {
		return false;
	}
	command = dbyPmicChannel(kPmicChn, apid);
	command[kPmicWdata0 / 4] = static_cast<uint32_t>(value);
	command[kPmicCmd / 4] = (static_cast<uint32_t>(kPmicOpWrite) << 27) |
	    (static_cast<uint32_t>(address) << 4);
	if (!dbyPmicWait(command)) {
		gPmic.errors++;
		return false;
	}
	return true;
}

/* Map the arbiter and resolve the two APIDs.  Same windows, same generation test and the same
 * 512-entry clamp as dby_spmi_set_bootloader(), which is the copy already exercised on this board. */
static bool
dbyPmicUp(void)
{
	static const uint64_t kBase[4] = { 0x0C440000ULL, 0x0C40A000ULL, 0x0E600000ULL, 0x0C600000ULL };
	static const uint64_t kSize[4] = { 0x00004000ULL, 0x00026000ULL, 0x00100000ULL, 0x02000000ULL };
	volatile uint32_t *core;
	uint32_t version, apids;
	unsigned k;

	if (gPmic.tried) {
		return gPmic.ready;
	}
	gPmic.tried = true;
	gPmic.apidPon = -1;
	gPmic.apidGpio6 = -1;
	for (k = 0; k < 4; k++) {
		if (!mapPhys(kBase[k], kSize[k], &gPmic.win[k])) {
			IOLog("DbyButtons: spmi window %u did not map\n", k);
			return false;
		}
	}
	core = reinterpret_cast<volatile uint32_t *>(gPmic.win[kPmicCore].base);
	version = core[0];
	if (version >= 0x50000000u && version < 0x70000000u) {
		gPmic.mark = 5;
		gPmic.obsStride = 0x80;
		gPmic.apidStride = 0x10000;
		gPmic.apidMapOff = 0x900;
	} else if (version >= 0x70000000u && version < 0x80000000u) {
		gPmic.mark = 7;
		gPmic.obsStride = 0x20;
		gPmic.apidStride = 0x1000;
		gPmic.apidMapOff = 0x2000;
	} else {
		IOLog("DbyButtons: spmi arbiter %08x is a generation this code does not know\n", version);
		return false;
	}
	apids = core[0x04 / 4] & 0x7ffu;
	if (apids == 0) {
		IOLog("DbyButtons: spmi reports no APIDs\n");
		return false;
	}
	if (gPmic.apidMapOff + 4u * apids > static_cast<uint32_t>(kSize[kPmicCore])) {
		apids = static_cast<uint32_t>((kSize[kPmicCore] - gPmic.apidMapOff) / 4u);
	}
	if (apids > 512u) {
		apids = 512u;
	}
	gPmic.apidCount = apids;
	gPmic.apidPon = dbyPmicApid(kPmicPpidPon);
	gPmic.apidGpio6 = dbyPmicApid(kPmicPpidGpio6);
	gPmic.ready = true;
	IOLog("DbyButtons: spmi v%u apids=%u pon=%d gpio6=%d\n", gPmic.mark, apids,
	    gPmic.apidPon, gPmic.apidGpio6);
	return true;
}

static void
dbyButtonsThread(void *arg, wait_result_t wait)
{
	uint32_t stable = 0;
	uint32_t candidate = 0;
	uint32_t count = 0;

	(void)arg;
	(void)wait;
	for (;;) {
		uint8_t pon = 0;
		uint8_t gpio = 0;
		bool okPon = dbyPmicRead(gPmic.apidPon, kPmicRegRtSts, &pon);
		bool okGpio = dbyPmicRead(gPmic.apidGpio6, kPmicRegRtSts, &gpio);
		uint32_t now = 0;

		if (okPon) {
			gPmic.lastPon = pon;
			if (pon & 0x01u) {
				now |= kBtnReportPower;         /* KPDPWR_N */
			}
			if (pon & 0x02u) {
				now |= kBtnReportVolDown;       /* RESIN_N  */
			}
		}
		if (okGpio) {
			gPmic.lastGpio = gpio;
			if ((gpio & 0x01u) == 0) {
				now |= kBtnReportVolUp;         /* ACTIVE_LOW pad */
			}
		}
		/* A failed read says nothing about the keys: keep the last state instead of
		 * publishing a release that never happened. */
		if (okPon || okGpio) {
			if (now == candidate) {
				if (count < static_cast<uint32_t>(kBtnStableFrames)) {
					count++;
				}
			} else {
				candidate = now;
				count = 1;
			}
			if (count >= static_cast<uint32_t>(kBtnStableFrames) && candidate != stable) {
				stable = candidate;
				gBtnMask = stable;
				if (gBtnSink != nullptr) {
					gBtnSinkCalls++;
					gBtnSink(gBtnSinkCtx, stable);
				}
			}
		}
		snprintf(native_touch_rows[3], sizeof(native_touch_rows[3]),
		    "BTN pon=%02x g6=%02x mask=%x apid=%d/%d v%u err=%u sink=%u",
		    static_cast<unsigned>(gPmic.lastPon), static_cast<unsigned>(gPmic.lastGpio),
		    static_cast<unsigned>(gBtnMask), gPmic.apidPon, gPmic.apidGpio6,
		    static_cast<unsigned>(gPmic.mark), static_cast<unsigned>(gPmic.errors),
		    static_cast<unsigned>(gBtnSinkCalls));
		IOSleep(kBtnPollMs);
	}
}

extern "C" void
dby_buttons_start(void)
{
	unsigned want = 0;
	thread_t thread = nullptr;

	if (gBtnStarted) {
		return;
	}
	if (!PE_parse_boot_argn("sheng-buttons", &want, sizeof(want)) || want != 1u) {
		return;
	}
	gBtnStarted = true;
	if (!dbyPmicUp()) {
		snprintf(native_touch_rows[3], sizeof(native_touch_rows[3]),
		    "BTN spmi unavailable (see log)");
		return;
	}
	/* GPIO6 is the volume-up pad.  Normal function, digital input, 30 uA pull-up on the
	 * 1.8 V rail: the three registers pinctrl-spmi-gpio.c writes for the dts node
	 * (input-enable / bias-pull-up / power-source = <1>) at kona-pmic-overlay.dtsi:69-77. */
	if (gPmic.apidGpio6 >= 0) {
		(void)dbyPmicWrite(gPmic.apidGpio6, kPmicRegVinCtl, 1);
		(void)dbyPmicWrite(gPmic.apidGpio6, kPmicRegPullCtl, 0);
		(void)dbyPmicWrite(gPmic.apidGpio6, kPmicRegModeCtl, 0);
	}
	if (kernel_thread_start(dbyButtonsThread, nullptr, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		snprintf(native_touch_rows[3], sizeof(native_touch_rows[3]), "BTN thread start failed");
	}
}

/* The hook NativeHidInput.cpp:305 calls once its IOHIDDevice is up.  Report 2's three
 * button bits are published from the poller above whenever the set changes. */
extern "C" void
native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context)
{
	gBtnSink = sink;
	gBtnSinkCtx = context;
}

/* ------------------------------------------------------------------ */
/* USB (DWC3) inheritance probe, read-only                            */
/* ------------------------------------------------------------------ */

/* Why: the CDC-ACM console (QcomXnuUsbDebug) may only take over a controller whose clocks,
 * power domain, reset and mode the firmware left usable.  On 2026-11-01 this board enabled that
 * console, the host never enumerated 1209:316d, and every tee'd printf went on to poll a
 * controller that had never come up; pulling the cable left the poll with no owner.  That
 * attempt recorded the symptom but never the inherited state.
 *
 * The UEFI this board runs does bring USB up: Platforms/Huawei/dby_w09Pkg/Include/DXE.inc:89-105
 * builds UsbfnDwc3Dxe (HS PHY + device stack), UsbDeviceDxe, UsbConfigDxe and UsbInitDxe, and
 * UsbInitDxe stops the controller again from its ExitBootServices callback.  The open question
 * is exactly what survives that handover.
 *
 * This probe answers it and writes nothing: no clock vote, no BCR reset, no GDSCR collapse and
 * no DWC3 store.  A block that is not clocked reads back 0xFFFFFFFF, and these rows show that
 * value rather than hiding it.
 *
 * Register sources (QcomXnuUsbDebug@ee80ec9a):
 *   soc/Sm8250.h      QUD_GCC_BASE 0x00100000 / 0x1f0000, QUD_DWC3_BASE 0x0a600000 / 0x100000,
 *                     QUD_GCC_CBCR_OFFSETS {0xf010,0xf018,0xf01c,0xf054,0xf058},
 *                     QUD_GCC_BCR_OFFSET 0xf000, QUD_GCC_GDSCR_OFFSET 0xf004,
 *                     QUD_GCC_CFG_GDSCR_OFFSET 0xf008, QUD_GDSC_POWER_BIT_LEGACY 31,
 *                     QUD_GCC_CBCR_REQUIRED 3
 *   usb/Dwc3Regs.h    GCTL 0xc110 (PRTCAP bits 13:12, 2 = device), GSNPSID 0xc120,
 *                     GHWPARAMS0 0xc140 (address width in bits 31:24), DSTS 0xc70c
 */
#define DBY_USB_GCC_PHYS        0x00100000ULL
#define DBY_USB_GCC_BYTES       0x00054000ULL   /* covers 0xf000..0xf058 */
#define DBY_USB_CORE_PHYS       0x0a600000ULL
#define DBY_USB_CORE_BYTES      0x00100000ULL

#define DBY_USB_CBCR_MASTER     0x0f010ULL
#define DBY_USB_CBCR_SLEEP      0x0f018ULL
#define DBY_USB_CBCR_UTMI       0x0f01cULL
#define DBY_USB_CBCR_PHY_AUX    0x0f054ULL
#define DBY_USB_CBCR_PHY_COM    0x0f058ULL
#define DBY_USB_BCR             0x0f000ULL
#define DBY_USB_GDSCR           0x0f004ULL
#define DBY_USB_CFG_GDSCR       0x0f008ULL

#define DBY_USB_GCTL            0x0c110ULL
#define DBY_USB_GSNPSID         0x0c120ULL
#define DBY_USB_GHWPARAMS0      0x0c140ULL
#define DBY_USB_DSTS            0x0c70cULL

#define DBY_USB_CBCR_CLK_OFF    (1U << 31)
#define DBY_USB_GDSCR_PWR_ON    (1U << 31)   /* QUD_GDSC_POWER_BIT_LEGACY */

/* One probe row: start at offset 0, NUL-terminate at the end.  rowAppend* keep the 120-byte
 * cell bounded; the explicit terminator is what makes an unused row read as empty. */
static unsigned
usbRowStart(unsigned slot, const char *text)
{
	char *row = native_touch_rows[slot];
	unsigned at = rowAppend(row, 120, 0, text);

	row[at] = 0;
	return at;
}

static void
usbRowEnd(unsigned slot, unsigned at)
{
	native_touch_rows[slot][at < 120 ? at : 119] = 0;
}

static bool
dbyUsbInspect(void)
{
	DbyTouchMap gcc;
	DbyTouchMap core;
	uint32_t cbcr[5];
	uint32_t bcr, gdscr, cfg;
	uint32_t gctl = 0, snpsid = 0, hw0 = 0, dsts = 0;
	bool coreMapped;
	unsigned at, i;

	if (!mapPhys(DBY_USB_GCC_PHYS, DBY_USB_GCC_BYTES, &gcc)) {
		at = usbRowStart(4, "USB gcc map FAILED");
		usbRowEnd(4, at);
		return false;
	}
	cbcr[0] = rd32(gcc.base, DBY_USB_CBCR_MASTER);
	cbcr[1] = rd32(gcc.base, DBY_USB_CBCR_SLEEP);
	cbcr[2] = rd32(gcc.base, DBY_USB_CBCR_UTMI);
	cbcr[3] = rd32(gcc.base, DBY_USB_CBCR_PHY_AUX);
	cbcr[4] = rd32(gcc.base, DBY_USB_CBCR_PHY_COM);
	bcr   = rd32(gcc.base, DBY_USB_BCR);
	gdscr = rd32(gcc.base, DBY_USB_GDSCR);
	cfg   = rd32(gcc.base, DBY_USB_CFG_GDSCR);
	unmapWindow(&gcc);

	/* The DWC3 window is a separate mapping.  An unclocked wrapper reads 0xFFFFFFFF and that
	 * is reported as such instead of being mistaken for a version register. */
	coreMapped = mapPhys(DBY_USB_CORE_PHYS, DBY_USB_CORE_BYTES, &core);
	if (coreMapped) {
		gctl   = rd32(core.base, DBY_USB_GCTL);
		snpsid = rd32(core.base, DBY_USB_GSNPSID);
		hw0    = rd32(core.base, DBY_USB_GHWPARAMS0);
		dsts   = rd32(core.base, DBY_USB_DSTS);
		unmapWindow(&core);
	}

	at = usbRowStart(4, "USB id=");
	at = rowAppendHex8(native_touch_rows[4], 120, at, coreMapped ? snpsid : 0xFFFFFFFFu);
	at = rowAppend(native_touch_rows[4], 120, at, " gctl=");
	at = rowAppendHex8(native_touch_rows[4], 120, at, coreMapped ? gctl : 0xFFFFFFFFu);
	usbRowEnd(4, at);

	at = usbRowStart(5, "USB dsts=");
	at = rowAppendHex8(native_touch_rows[5], 120, at, coreMapped ? dsts : 0xFFFFFFFFu);
	at = rowAppend(native_touch_rows[5], 120, at, " hw0=");
	at = rowAppendHex8(native_touch_rows[5], 120, at, coreMapped ? hw0 : 0xFFFFFFFFu);
	usbRowEnd(5, at);

	at = usbRowStart(6, "USB gd=");
	at = rowAppendHex8(native_touch_rows[6], 120, at, gdscr);
	at = rowAppend(native_touch_rows[6], 120, at, " cfg=");
	at = rowAppendHex8(native_touch_rows[6], 120, at, cfg);
	usbRowEnd(6, at);

	/* Verdict row, in the wording of the checks that gate takeover:
	 *   v1..v5  one digit per CBCR, 1 = CLK_OFF clear.  Only the first QUD_GCC_CBCR_REQUIRED
	 *           (3) have to be 1 here; the two PHY auxiliary branches may read 0 because the
	 *           SS PHY is unused on this board.
	 *   b       BCR bit0; 1 means the block reset is still asserted -> QusbResetAsserted.
	 *   pwr     GDSCR bit31 PWR_ON (SM8250 has no qcom,support-cfg-gdscr, so this bit, not
	 *           CFG_GDSCR bit16, is the module's power verdict).
	 *   mode    GCTL bits 13:12; 2 = device, 1 = host, 3 = OTG.
	 *   w       GHWPARAMS0 bits 31:24, the DWC3 master address width. */
	at = usbRowStart(7, "USB v=");
	for (i = 0; i < 5; ++i) {
		native_touch_rows[7][at++] = ((cbcr[i] & DBY_USB_CBCR_CLK_OFF) == 0) ? '1' : '0';
	}
	at = rowAppend(native_touch_rows[7], 120, at, " b=");
	at = rowAppendDec(native_touch_rows[7], 120, at, bcr & 1u);
	at = rowAppend(native_touch_rows[7], 120, at, " pwr=");
	at = rowAppendDec(native_touch_rows[7], 120, at, (gdscr & DBY_USB_GDSCR_PWR_ON) ? 1u : 0u);
	at = rowAppend(native_touch_rows[7], 120, at, " mode=");
	at = rowAppendDec(native_touch_rows[7], 120, at, coreMapped ? ((gctl >> 12) & 3u) : 9u);
	at = rowAppend(native_touch_rows[7], 120, at, " w=");
	at = rowAppendDec(native_touch_rows[7], 120, at, coreMapped ? ((hw0 >> 24) & 0xffu) : 0u);
	usbRowEnd(7, at);

	return true;
}

extern "C" int
dby_touch_probe_entry(void)
{
	/* Buttons carry their own gate and must work on a boot that never starts the touch
	 * ladder, so this runs before the stage test. */
	dby_buttons_start();

	/* USB inheritance probe: its own gate, read-only, and deliberately before the stage test so
	 * a payload that never starts the touch ladder can still answer the question. */
	{
		uint32_t usbWant = 0;

		if (PE_parse_boot_argn("dby-usb-probe", &usbWant, sizeof usbWant) && usbWant == 1u) {
			(void)dbyUsbInspect();
			publishPageRow();
		}
	}

	uint32_t stage = parseStage();

	/*
	 * Stage 0 must be completely inert.  The board calls this entry unconditionally from
	 * boot_progress_poll(), so mapping the three MMIO windows "just to census" would touch
	 * hardware on a default boot that never asked for this driver.  Return before any mapping;
	 * DBY_TOUCH_OK is what lets the caller mark the probe as finished instead of retrying.
	 */
	if (stage == DBY_TOUCH_STAGE_OFF) {
		return DBY_TOUCH_OK;
	}

	gState.stage = stage;
	if (!mapWindows()) {
		IOLog("DbyTouch(entry): mapping failed\n");
		publishPageRow();
		return (int)DBY_TOUCH_ERR_IO;
	}
	/* Judgment experiment (2026-10-31).  The pin census reads 0x00000000 for every pin even after
	 * the board described the pin controller, so the open question is whether the mapping idiom
	 * works at all on this platform or whether that one window simply carries no hardware.  Read
	 * three windows through the same mapPhys():
	 *   0x17a00000  GIC distributor, GICD_CTLR  (live: it delivers the interrupts counted above)
	 *   0x1d84000   UFS host, HCI version at +0x08 (live: the root volume is on it; read-only)
	 *   0x0f100000  pin controller, GPIO_CFG39  (the window under test)
	 * Sources: dts/sm8250.dtsi (gic@17a00000, ufshc@1d84000), DbyTouchBoard.h:144-146 (TLMM).
	 * The result goes on native_touch_rows[1], which BootProgress.c draws on grid row 47 and the
	 * log writer therefore archives every two seconds.
	 * This runs in the entry path only: the IOKit personality is not landed on this board, so
	 * DbyTouch::start() never executes and must not be the place for it.
	 */
	{
		DbyTouchMap probe = {};
		if (mapPhys(0x17a00000ULL, 0x1000, &probe)) {
			gProbeGic = rd32(probe.base, 0);
			unmapWindow(&probe);
		}
		if (mapPhys(0x1d84000ULL, 0x1000, &probe)) {
			gProbeUfs = rd32(probe.base, 0x08);
			unmapWindow(&probe);
		}
		{
			DbyTouchMap *irqTile = tlmmTile(DBY_TOUCH_GPIO_IRQ);

			if (irqTile != nullptr && irqTile->base != 0) {
				gProbeTlmm = rd32(irqTile->base,
				    static_cast<uint64_t>(DBY_TOUCH_GPIO_IRQ) * DBY_TOUCH_TLMM_STRIDE);
			} else {
				gProbeTlmm = DBY_TOUCH_PROBE_FAIL;
			}
		}
		/* Second round.  Two mistakes the first attempt made, both fixed:
		 *   * mapPhys() only creates a mapping; it does not check that the length covers the offset
		 *     read afterwards.  A 0x1000 mapping plus rd32(base, 0x77024) reads past the mapping
		 *     (data abort).  Map the whole span once instead.
		 *   * a value of 0 cannot be told apart from a failed mapping, so a failure now records
		 *     DBY_TOUCH_PROBE_FAIL and the three pin windows are read at the SAME offset.
		 *
		 * GCC 0x00100000 + 0x77024: a branch-enable register transcribed from the SM8550 clock
		 * driver (GccUfs.h:16).  SM8250/kona has no local evidence for that offset, so a non-zero
		 * read only proves "the clock controller window answers"; a zero is not conclusive.
		 * TLMM: kona's pin windows are 0x0F100000 (west), 0x0F500000 (south), 0x0F900000 (north)
		 * (uefi-dby_w09 MemoryMapLib.c:92-94).  Reading pin 39 in all three separates "only the west
		 * window answers zero" from "the pin controller answers zero everywhere".
		 */
		if (mapPhys(0x00100000ULL, 0x80000ULL, &probe)) {
			gProbeGcc = rd32(probe.base, 0x77024);
			unmapWindow(&probe);
		} else {
			gProbeGcc = DBY_TOUCH_PROBE_FAIL;
		}
		/* Where are the pin registers?
		 *
		 * The device's own base DTB describes ONE pin controller: `pinctrl@f000000`,
		 * `reg = <0xf000000 0x1000000>` (evidence/derived/kona-base-padded.dtb), whereas mainline
		 * sm8250 uses `pinctrl@f100000` with 0x300000.  The driver has always used 0x0F100000 and
		 * read zeros there, so scan the block the board itself declares: walk 0x1000 pages over the
		 * low 2 MiB and record how many read non-zero and where the first one is.  A page that
		 * carries pins has a plausible GPIO_CFG word (func in bits [5:2]); pages of unused address
		 * space read zero, which is exactly what the current base returns.
		 */
		/* The firmware reads the pin controller before XNU starts and hands the value over as the
		 * boot argument `dby-pinprobe` (UniXNUPkg/Library/UniXNULib/Main.c).  That is the only way
		 * this board can see the pin registers: here every GPIO_CFG word reads 0x00000000 and
		 * scanning the window hung the kernel (2026-10-31), while the firmware has the west window
		 * mapped.  A scan is deliberately not attempted again. */
		{
			uint32_t firmware = 0;
			if (PE_parse_boot_argn("dby-pinprobe", &firmware, sizeof firmware)) {
				gPinProbeFromFirmware = firmware;
				gPinProbeSeen = true;
			}
		}
		{
			DbyTouchMap *irqTile = tlmmTile(DBY_TOUCH_GPIO_IRQ);

			if (irqTile != nullptr && irqTile->base != 0) {
				gScanWestVal = rd32(irqTile->base,
				    static_cast<uint64_t>(DBY_TOUCH_GPIO_IRQ) * DBY_TOUCH_TLMM_STRIDE);
			}
		}
	}
	census();
	/* Stage 2 used to apply the pin plan only on the IOService path, so
	 * `dby-touch=2` through this entry point logged "done at stage 2" without
	 * writing anything.  Both paths now run it. */
	if (stage >= DBY_TOUCH_STAGE_CLOCKS) {
		if (!gStage2Started) {
			thread_t stage2;

			gStage2Started = true;
			if (kernel_thread_start(stage2Thread, NULL, &stage2) != KERN_SUCCESS) {
				gStage2Started = false;
				IOLog("DbyTouch(entry): stage 2 thread failed to start\n");
			} else {
				thread_deallocate(stage2);
			}
		}
	} else if (stage != DBY_TOUCH_STAGE_OFF) {
		IOLog("DbyTouch(entry): stage %u < %u, no register written\n", stage,
		    (unsigned)DBY_TOUCH_STAGE_CLOCKS);
	}
	publishPageRow();
	IOLog("DbyTouch(entry): done at stage %u\n", stage);
	return DBY_TOUCH_OK;
}
