// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off staged bring-up of the NT36532E touch controller. `sheng-touch=<n>`:
//   1  read-only snapshot of the GCC branches, the S4 DFS levels and the touch pins
//   2  vote the QUP clocks, wait for them, configure the pins, read the GENI engine identity
//   3  GENI SPI init and clock, then reset the touch bootloader and read the chip id
//   4  host-download the firmware, start scanning and process THP frames (needs the generated firmware object)
// Every stage reports on the diagnostic page; the first failure stops the sequence. Nothing here is validated on the
// device until a boot has shown it. Board constants and their sources are in TouchBoard.h.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <string.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "GeniSpi.h"
#include "NvtProtocol.h"
#include "NvtTouchCore.h"
#include "TouchBoard.h"
#include "SmmuBypass.h"
extern const uint8_t native_touch_firmware[] __attribute__((weak_import));
extern const uint32_t native_touch_firmware_size __attribute__((weak_import));
extern const uint8_t native_touch_firmware_alt[] __attribute__((weak_import));
extern const uint32_t native_touch_firmware_alt_size __attribute__((weak_import));
}

extern "C" {
char native_touch_rows[5][120];
char native_touch_trace[8][120];
volatile uint32_t native_touch_loop_step;
volatile uint32_t native_touch_stage_req, native_touch_stage_done, native_touch_step, native_touch_frames,
    native_touch_valid, native_touch_contacts, native_touch_hid_reports;
volatile int native_touch_error;
volatile int native_touch_last_x = -1, native_touch_last_y = -1;
// Latest finger set for the HID digitizer source (see NativeHidInput); sequence is odd while being written.
volatile uint32_t native_touch_seq;
volatile uint32_t native_touch_count;
volatile int native_touch_xs[NVT_SLOTS], native_touch_ys[NVT_SLOTS], native_touch_ids[NVT_SLOTS];
// Finished strokes as published (int-150, for edge gestures that did nothing in int-148/149): a ring of {start ms, duration ms, first x, first y,
// last x, last y, frames} in published units (0.1 px, y already mirrored), and the extent every published contact reached {min x, max x,
// min y, max y}. Read by kits/sheng BootLog.c.
volatile int32_t native_touch_strokes[16][7];
volatile uint32_t native_touch_stroke_count;
volatile int32_t native_touch_extent[4] = {0x7fffffff, -1, 0x7fffffff, -1};
// The consumer's own counters, defined in ShengHidInput's NativeHidInput.cpp (namespace edge). docs/porting/input.md states
// the expected result as "the OS receives correct down, move and up reports for each contact, and UI gestures follow the panel
// orientation", so these - not any panel-side decode count - are the verdict row:
//   edge_stats[0] hook installed on the event driver, [1] contacts begun, [2] of them carrying kIOHIDDigitizerEventFromEdgeTip,
//   [3] events skipped; hid_multi[0] reports with two or more fingers, [1] the most fingers in one report, [2] tip-up entries
//   sent for fingers that vanished. uke's heartbeat prints the same set (drv/rpt/edge, docs/TOUCH-PORT.md:55-58).
extern volatile uint32_t native_touch_edge_stats[4];
extern volatile uint32_t native_touch_hid_multi[3];
// Every gate the contact set has to pass on the OS side, from package/HidInputProbe.cpp (device creation) and
// NativeHidInput.cpp (report submission), in this order: the HID family must be present, a provider must be there to
// attach under (rs=1 real IOHIDResource, pv=2 the IOResources fallback), the IOHIDDevice personality must match, the
// service must start, an IOHIDEventDriver must attach below it, and only then can a report be submitted. Printing all of
// them at once is what makes one readout answer "which gate is closed" - the panel side cannot show any of this.
extern volatile uint32_t native_hid_stage, native_hid_resource_found, native_hid_provider_used, native_hid_class_sizes[5], native_hid_input_events[8];
extern volatile int native_hid_load_status, native_hid_input_meta_status, native_hid_input_results[8];
// Set by native_touch_set_sink() and by the sink call in publishRuler(): together these say whether
// NativeHidInput::start() ever handed us its callback, and how many frames we actually passed to it.
volatile uint32_t native_touch_sink_set, native_touch_notify_calls;
// Non-zero while the scripted link test (sheng-swipe) holds a contact down.
volatile uint32_t gScriptedDown;
// package/HidInputProbe.cpp: 1 = ask the HID device for one Home press on its next 1 s tick.
extern volatile uint32_t native_hid_input_command;
// package/LegacyDisplayProbe.cpp: frames the display has presented, so >= 5 means the UI is up. The two counters answer the
// same question on the two hosts: transfer_calls ticks only in the copy-to-scanout path the hardware uses, while frame_count is
// the official "a picture appeared" counter and also ticks when the UI is rendered CPU-side (offline QEMU has no swap path).
extern volatile uint32_t native_legacy_transfer_calls, native_legacy_frame_count;
}

namespace {
struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};

bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

// The serial engine's DMA master reads and writes two 64 KiB buffers. The apps SMMU is firmware-managed; the stream gets the
// same pass-through mapping the UFS driver uses, so the engine addresses physical memory. Caches are maintained explicitly:
// the DT marks the wrapper dma-coherent, but whether bypass traffic snoops is not assumed.
constexpr size_t kDmaBytes = 64 * 1024;
void cacheRange(void *p, size_t len, int op) {   // 0 clean, 1 clean+invalidate, 2 invalidate
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t line = 4u << ((ctr >> 16) & 15);
    uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(line - 1), end = reinterpret_cast<uintptr_t>(p) + len;
    for (; a < end; a += line) {
        if (op == 0) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
        else if (op == 1) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
void dmaClean(void *, void *p, size_t len) {cacheRange(p, len, 0);}
void dmaCleanInval(void *, void *p, size_t len) {cacheRange(p, len, 1);}
void dmaInval(void *, void *p, size_t len) {cacheRange(p, len, 2);}
uint32_t smmuRd(void *ctx, unsigned o) {return rd32(reinterpret_cast<uintptr_t>(ctx), o);}
void smmuWr(void *ctx, unsigned o, uint32_t v) {wr32(reinterpret_cast<uintptr_t>(ctx), o, v);}
int smmuErrorCode(const char *e) {
    static const char *const names[] = {"SMMU AArch64 stage 1 unsupported", "DMA exceeds SMMU address width", "unsupported SMMU geometry",
        "conflicting stream mapping", "reserved context bank is not a bypass bank", "no free stream-match group",
        "SMMU bypass bank readback", "SMMU stream context readback", "SMMU stream mapping readback"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) if (strcmp(e, names[i]) == 0) return static_cast<int>(i) + 1;
    return 99;
}

uint32_t ioRd(void *ctx, uint32_t reg) {return rd32(reinterpret_cast<uintptr_t>(ctx), reg);}
void ioWr(void *ctx, uint32_t reg, uint32_t value) {wr32(reinterpret_cast<uintptr_t>(ctx), reg, value);}
void ioDelayUs(void *, unsigned us) {IODelay(us);}

struct Session {
    Region gccLow, gccS4, gccHalt, gccVote, tlmmIrq, tlmmSpi, wrapper, se, smmu;
    IOBufferMemoryDescriptor *dmaMem;
    uint64_t dmaPhys;
    int smmuSlot, smmuBank, smmuErr;
    // First transfers as seen on the wire (tx = bytes the driver sent, rx = bytes the engine returned), and the identification
    // result per SPI mode tried.
    unsigned traceN;
    uint32_t traceLen[8];
    uint8_t traceTx[8][8], traceRx[8][10];
    unsigned modeTried, modeUsed;
    // Last six transfers (ring) and the firmware being downloaded, for failures after identification.
    unsigned ringN;
    uint32_t ringLen[6];
    uint8_t ringTx[6][8], ringRx[6][8];
    unsigned fwParts, fwCasc, fwIlm, fwDlm, fwSize;
    uint8_t fwImage;                  // which embedded image last reached the download: 1 primary, 2 alternate
    uint32_t fwTried;                 // images offered to the part at all
    // Frame loop evidence.
    uint32_t reads, bad, irqHigh, irqSeen, irqIdle, peeks;
    uint8_t head[24], thp[24];
    int mn, mx;
    int loopRc = 99;                  // engine loopback self-test (99 = not run)
    uint8_t loopRx[16];
    uint32_t loopTxIn, loopRxIn;
    uint8_t loopRxLate[8];
    uint32_t reg[16];                  // engine registers read back after the loopback transfer (what the writes left behind)
    uint8_t modeId[4][6];
    int modeRc[4];
    struct geni_spi spi;
    uint8_t *tx = nullptr, *rx = nullptr, *frame = nullptr;
    int32_t *matrix = nullptr;       // 16 x 50 int32 touch matrix (heap: kernel thread stacks are 16 KiB)
    // One vendor-shaped frame read (novatek_thp.c touch_driver_get_frame: command byte 0x20, the reply of that same
    // full-duplex transfer is the frame). `aligned` counts bursts that carry the header gate this panel's own library
    // applies - A5 5A 5A A5 - and `sync` is where that burst starts, `syncHist` every offset that ever held it: the
    // number of leading bytes is a property of this die, so it is measured, never chosen.
    uint32_t bursts, burstRc, aligned, fresh, lastCounter, lastType, syncHist[16];
    int sync = -1;
    uint32_t matrixPub, slotFrames;   // matrix passes that found a contact; bursts the slot decoder believed
    // The payload's own layout is scored the same way, because the factory library anchors it at a table-derived
    // offset it never prints: {48,51,54,57} bytes from the burst start (the 54-byte header give or take the 3-byte
    // command prefix rwSPIBuf adds) x the six node unfoldings nvt_thp_nodes knows. Every candidate is measured on
    // real frames by the vendor's own acceptance rule (neighbouring nodes must be close) and the smoothest wins;
    // `voted` counts the frames that scored, `roughSkipped` the candidates that would have read past the burst.
    uint8_t *alt = nullptr;           // second 800-node grid: candidate scoring, then the negated feed
    struct nvt_touch_core *coreNeg = nullptr;   // the same frame with the other sign (see burstPass)
    uint8_t lastPayload[NVT_THP_PAYLOAD_LEN];   // replay test on the bytes themselves, not on a header field
    uint8_t haveLast, matrixLive;
    uint32_t replay, voted, roughSkipped;
    uint32_t roughSum[4][NVT_THP_V_KINDS];
    unsigned payloadOff, variant, polarity, polaritySet;
    // The vendor's own point-report branch (nt36xxx_ops.c novatek_irq_bottom_half), run on every burst that carries no
    // THP header. `pointPub` counts the reports published, `pointArm` the ones seen once but not yet believed.
    uint32_t pointPub, pointArm, pointBase, pointContacts;
    // The same vendor report read the way the vendor reads it. Its IRQ bottom half does not reinterpret the 0x20 burst:
    // it performs one paged read of POINT_AFT_DATA_LEN bytes from the firmware's own address and parses six-byte slots
    // from byte 1 (ksrc/nt36xxx_ops.c:68 with the range comment 0x11E00 ~ 0x11E98, :2752, :2763, :2807). Which base a
    // die uses is documented as varying, so every candidate is polled and counted instead of guessed.
    uint8_t *pref = nullptr;
    uint32_t prefPub, prefArm, prefContacts, prefSeen[4], prefBusErr, prefWdt;
    uint8_t prefLive, prefLastIds[10];
    int prefLastN;
    // The spans the point decoder bounds-check against - the vendor uses abs_x_max/abs_y_max out of the firmware info
    // block (nt36xxx_ops.c:2825), never a constant. 0 means the info block never answered and the devicetree's own
    // abs_max_x/abs_max_y are used; `spanSrc` records which of the two a readout was decoded with.
    uint32_t spanX, spanY, spanSrc;
    uint32_t wdt;                     // bursts holding the vendor's watchdog pattern (bytes 1..6 all 0xFD/0xFE)
    int lastPointIds[10], lastPointN; // the previous believed report, for the second-sight rule below
    unsigned fwState;                 // 0 none, 1 already streaming, 2 revived by a restart byte, 3 revived by a download
    uint8_t restartByte;
    int dlRc = 0;
    struct nvt_chip chip;
    struct nvt_bus bus;
    struct nvt_touch_core *core = nullptr;
    uint32_t vals[40];
    int lastRc = 0;
    uint32_t lastLen = 0, xIrq = 0, xGeni = 0, xTx = 0, xRx = 0;
    uint64_t levelHz[TB_DFS_LEVELS];
    unsigned level = 0, div = 0;
    uint64_t actualHz = 0;
};

int busXfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    Session *s = static_cast<Session *>(ctx);
    int rc = geni_spi_transfer(&s->spi, tx, rx, len, 0);
    {
        unsigned i = s->ringN++ % 6;
        s->ringLen[i] = static_cast<uint32_t>(len);
        for (unsigned k = 0; k < 8; ++k) {
            s->ringTx[i][k] = (tx && k < len) ? tx[k] : 0;
            s->ringRx[i][k] = (rx && k < len && !rc) ? rx[k] : 0;
        }
    }
    if (!rc && s->traceN < 8) {
        unsigned i = s->traceN++;
        s->traceLen[i] = static_cast<uint32_t>(len);
        for (unsigned k = 0; k < 8 && k < len && tx; ++k) s->traceTx[i][k] = tx[k];
        for (unsigned k = 0; k < 10 && k < len && rx; ++k) s->traceRx[i][k] = rx[k];
    }
    if (rc) {   // keep the engine state of the first failure
        if (!s->lastRc) {
            s->lastRc = rc;
            s->lastLen = static_cast<uint32_t>(len);
            // Registers read by the driver just before it cancelled the command (reading them here would show the
            // post-recovery state).
            s->xIrq = s->spi.snap_irq;
            s->xGeni = s->spi.snap_status;
            s->xTx = s->spi.snap_txfs;
            s->xRx = s->spi.snap_rxfs;
        }
    }
    return rc;
}
void busDelay(void *, unsigned ms) {IOSleep(ms);}

void fail(unsigned step, int code) {
    if (!native_touch_error) {native_touch_step = step; native_touch_error = code ? code : -1;}
}

void rows(const Session &s);
void mark(const Session &s, unsigned step) {native_touch_step = step; rows(s);}

// snprintf returns the length it would have written, so a running offset must be clamped before the next append.
struct Out {
    char *p; size_t cap, n;
    void add(const char *fmt, unsigned a = 0, unsigned b = 0) {
        if (n + 1 >= cap) return;
        int w = snprintf(p + n, cap - n, fmt, a, b);
        if (w < 0) return;
        n = (n + static_cast<size_t>(w) >= cap) ? cap - 1 : n + static_cast<size_t>(w);
    }
    void hex(uint8_t v) {add("%02x", v);}
};

void traceRows(const Session &s) {
    for (unsigned r = 0; r < 2; ++r) {
        Out o = {native_touch_trace[r], sizeof native_touch_trace[r], 0};
        o.p[0] = 0;
        o.add("TCH xfer");
        for (unsigned j = 0; j < 2; ++j) {
            unsigned i = r * 2 + j;
            if (i >= s.traceN) break;
            o.add(" #%u len%u tx", i, s.traceLen[i]);
            for (unsigned k = 0; k < 8 && k < s.traceLen[i]; ++k) o.hex(s.traceTx[i][k]);
            o.add(" rx");
            for (unsigned k = 0; k < 10 && k < s.traceLen[i]; ++k) o.hex(s.traceRx[i][k]);
            o.add(" |");
        }
    }
    const struct nvt_chip &c = s.chip;
    Out v = {native_touch_trace[2], sizeof native_touch_trace[2], 0};
    v.p[0] = 0;
    v.add("TCH sram-readback ilm bad%u", c.sram_bad[0]);
    v.add(" first%d", static_cast<unsigned>(c.sram_first_bad[0]));
    v.add(" crc%08x", c.sram_crc[0]);
    v.add(" | dlm bad%u", c.sram_bad[1]);
    v.add(" first%d", static_cast<unsigned>(c.sram_first_bad[1]));
    v.add(" crc%08x", c.sram_crc[1]);
    Out f = {native_touch_trace[3], sizeof native_touch_trace[3], 0};
    f.p[0] = 0;
    f.add("TCH frames lp%u", native_touch_loop_step);
    f.add(" peek%u", s.peeks);
    f.add(" new%u", s.fresh);
    f.add(" type%u", s.lastType);
    f.add(" reads%u", s.reads);
    f.add(" bad%u", s.bad);
    f.add(" irq%u", s.irqHigh);
    f.add(" seen%u", s.irqSeen);
    f.add(" min%d", static_cast<unsigned>(s.mn));
    f.add(" max%d head", static_cast<unsigned>(s.mx));
    for (unsigned k = 0; k < 24; ++k) f.hex(s.head[k]);
    Out d = {native_touch_trace[4], sizeof native_touch_trace[4], 0};
    d.p[0] = 0;
    d.add("TCH dl try%u", c.dl_attempt);
    d.add(" stage%u", c.dl_stage);
    d.add(" rc%d", static_cast<unsigned>(c.dl_rc));
    d.add(" wait%02x", c.dl_wait_value);
    d.add(" polls%u rst", c.dl_polls);
    for (unsigned k = 0; k < 5; ++k) d.hex(c.dl_reset_state[k]);
    d.add(" fw parts%u", s.fwParts);
    d.add(" casc%u", s.fwCasc);
    d.add(" ilm%u", s.fwIlm);
    d.add(" dlm%u", s.fwDlm);
    for (unsigned r = 0; r < 2; ++r) {
        Out o = {native_touch_trace[5 + r], sizeof native_touch_trace[5 + r], 0};
        o.p[0] = 0;
        o.add("TCH last");
        for (unsigned j = 0; j < 2; ++j) {
            unsigned k = r * 2 + j;                       // last four, oldest first
            if (s.ringN < 4 && k >= s.ringN) break;
            unsigned seq = s.ringN >= 4 ? s.ringN - 4 + k : k;
            unsigned i = seq % 6;
            o.add(" #%u len%u tx", seq, s.ringLen[i]);
            for (unsigned q = 0; q < 8 && q < s.ringLen[i]; ++q) o.hex(s.ringTx[i][q]);
            o.add(" rx");
            for (unsigned q = 0; q < 8 && q < s.ringLen[i]; ++q) o.hex(s.ringRx[i][q]);
            o.add(" |");
        }
    }
    Out t = {native_touch_trace[7], sizeof native_touch_trace[7], 0};
    t.p[0] = 0;
    t.add("TCH thp-header");
    for (unsigned q = 0; q < 24; ++q) t.hex(s.thp[q]);
}

void rows(const Session &s) {
    traceRows(s);
    const uint32_t *v = s.vals;
    // The only row this board draws is rows[0] (BootProgress prints that one line on the diagnostic page, and the page is
    // gone once the UI takes over), so it carries the verdict in gate order: did a burst come back, does it hold the
    // marker this panel's own library checks, where, which layout the nodes turned out to be, how many frames were new
    // rather than replays, what the decoder made of them and which sign it needed. b<mn>/<mx> is the raw node band, so
    // a geometry mistake is visible as a band that is not the ~3,700 the caplimit CSV declares.
    snprintf(native_touch_rows[0], sizeof native_touch_rows[0],
        "TCH s%u/%u e%d fw%u sy%d/%x al%u/%u v%u.%u f%u rp%u c%u %d,%d b%d/%d pp%u nb%u sp%u:%u/%u",
        native_touch_stage_done, native_touch_stage_req, native_touch_error,
        s.fwState, s.sync,
        (s.syncHist[0] ? 1u : 0u) | (s.syncHist[1] ? 2u : 0u) | (s.syncHist[2] ? 4u : 0u) | (s.syncHist[3] ? 8u : 0u) |
        (s.syncHist[4] ? 16u : 0u) | (s.syncHist[5] ? 32u : 0u) | (s.syncHist[6] ? 64u : 0u) | (s.syncHist[7] ? 128u : 0u),
        s.aligned, s.bursts, s.variant, s.payloadOff, s.fresh, s.replay,
        native_touch_contacts, native_touch_last_x, native_touch_last_y, s.mn, s.mx,
        s.pointPub, s.prefPub,
        s.spanSrc, s.spanX, s.spanY);
    snprintf(native_touch_rows[1], sizeof native_touch_rows[1],
        "TCH p%08x %08x %08x %08x irq%08x wr%08x fw%08x if%08x hw%08x L%u/%u %uHz",
        v[9], v[10], v[11], v[12], v[13], v[14], v[15], v[16], v[17], s.level, s.div, (unsigned)s.actualHz);
    snprintf(native_touch_rows[2], sizeof native_touch_rows[2],
        "TCH id%02x%02x%02x%02x%02x%02x %.8s v%02x p%04x %ux%u fr%u ok%u mp%u sf%u st%u pol%u rb%02x dl%d/%u.%u bm%x dfs%08x",
        s.chip.id[0], s.chip.id[1], s.chip.id[2], s.chip.id[3], s.chip.id[4], s.chip.id[5],
        s.chip.map ? s.chip.map->name : "-", v[18], v[19], v[20], v[21],
        native_touch_frames, native_touch_valid, s.matrixPub, s.slotFrames,
        native_touch_step, s.polarity, s.restartByte, s.dlRc, s.fwTried, s.fwImage,
        (s.prefSeen[0] ? 1u : 0u) | (s.prefSeen[1] ? 2u : 0u) | (s.prefSeen[2] ? 4u : 0u) | (s.prefSeen[3] ? 8u : 0u), v[7]);
    snprintf(native_touch_rows[3], sizeof native_touch_rows[3],
        "TCH xf rc%d n%u ev%08x gs%08x tx%08x rx%08x dma t%x r%x ti%x ri%x sm%d/%d/e%d pa%llx",
        s.lastRc, s.lastLen, s.xIrq, s.xGeni, s.xTx, s.xRx, s.spi.snap_dma_tx, s.spi.snap_dma_rx, s.spi.snap_dma_txin,
        s.spi.snap_dma_rxin, s.smmuSlot, s.smmuBank, s.smmuErr, (unsigned long long)s.dmaPhys);
    // The verdict row: what the OS side actually did with the fingers this probe published. hid=0 with c>0 means the contacts
    // died between the panel and the event driver (no consumer attached, or the usage pair is not Digitizer/Touch Screen), and
    // no amount of decoder work can show that. One line, in gate order:
    //   st=HidInputProbe stage (3 = done)  ld=IOHIDFamily load status  rs=IOHIDResource found  pv=provider actually used (1 =
    //   that resource, 2 = the IOResources fallback, 0 = none)  sz=IOHIDDevice class size (208
    //   is the gate the device is created behind)  ms=metaclass postModLoad  snk=start() installed our sink  nf=frames we
    //   handed to that sink  in=start() completed  ed=IOHIDEventDriver found and edge hook installed  rpt=touchSend reached
    //   submitReport  r=its IOReturn (-1 = never ran)  hid=reports counted in the consumer  k=button-path reports (the same
    //   hasEventDriverCached() gate with a different input, so k>0 && rpt=0 puts the fault after that gate)
    //   seq/c=the published set itself  mf=multi-finger reports / most fingers in one report.
    snprintf(native_touch_rows[4], sizeof native_touch_rows[4],
        "HID st%u ld%d rs%u pv%u sz%u ms%d snk%u nf%u in%u ed%u rpt%u r%d hid%u k%u seq%u c%u mf%u/%u",
        native_hid_stage, native_hid_load_status, native_hid_resource_found, native_hid_provider_used,
        native_hid_class_sizes[4],
        native_hid_input_meta_status, native_touch_sink_set, native_touch_notify_calls,
        native_hid_input_events[2], native_touch_edge_stats[0], native_hid_input_events[6],
        native_hid_input_results[6], native_touch_hid_reports,
        native_hid_input_events[4] + native_hid_input_events[5], native_touch_seq, native_touch_count,
        native_touch_hid_multi[0], native_touch_hid_multi[1]);
}

struct Stroke {int id, x0, y0, x1, y1, frames; uint64_t t0;};
Stroke strokes[NVT_SLOTS];
unsigned strokeCount;
uint32_t touchMs() {
    uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return (uint32_t)(mach_absolute_time() / ((f ? f : 19200000) / 1000));
}
void trackStrokes(unsigned count, const int *ids, const int *xs, const int *ys) {
    const uint32_t now = touchMs();
    for (unsigned i = 0; i < count; ++i) {
        if (xs[i] < native_touch_extent[0]) native_touch_extent[0] = xs[i];
        if (xs[i] > native_touch_extent[1]) native_touch_extent[1] = xs[i];
        if (ys[i] < native_touch_extent[2]) native_touch_extent[2] = ys[i];
        if (ys[i] > native_touch_extent[3]) native_touch_extent[3] = ys[i];
        unsigned k = 0;
        while (k < strokeCount && strokes[k].id != ids[i]) ++k;
        if (k == strokeCount) {
            if (strokeCount == NVT_SLOTS) continue;
            strokes[strokeCount++] = {ids[i], xs[i], ys[i], xs[i], ys[i], 0, now};
        }
        strokes[k].x1 = xs[i]; strokes[k].y1 = ys[i]; ++strokes[k].frames;
    }
    for (unsigned k = 0; k < strokeCount;) {
        bool live = false;
        for (unsigned i = 0; i < count && !live; ++i) live = ids[i] == strokes[k].id;
        if (live) {++k; continue;}
        volatile int32_t *r = native_touch_strokes[native_touch_stroke_count & 15];
        r[0] = (int32_t)(uint32_t)strokes[k].t0; r[1] = (int32_t)(now - (uint32_t)strokes[k].t0);
        r[2] = strokes[k].x0; r[3] = strokes[k].y0; r[4] = strokes[k].x1; r[5] = strokes[k].y1; r[6] = strokes[k].frames;
        native_touch_stroke_count = native_touch_stroke_count + 1;
        strokes[k] = strokes[--strokeCount];
    }
}

// The HID driver (drivers/ShengHidInput) stopped polling this seqlock: it registers a sink at start() and expects the
// publisher to call it once per frame (NativeHidInput::touchNotify -> runAction -> touchSend). That is the only path a
// finger takes to the event driver, so the sink lives here, next to the publish it belongs to.
static void (*gTouchNotify)(void *);
static void *gTouchNotifyContext;

extern "C" void native_touch_set_sink(void (*sink)(void *), void *context) {
    gTouchNotifyContext = context;
    __atomic_store_n(&gTouchNotify, sink, __ATOMIC_RELEASE);
    if (sink) native_touch_sink_set = native_touch_sink_set + 1;
}

// The consumer this kernel links (drivers/ShengHidInput/NativeHidInput.cpp) normalises with two hard-coded maxima -
//   x = transformCoordinate(xs[i], 30480), y = transformCoordinate(ys[i], 20320)
//   where transformCoordinate(v, m) = clamp(v, 0, m-1) * 32767 / (m-1)
// - because those are its own board's touch-core axis maxima. It is a shared kit and is not edited per board, so every
// board converts on its publish side; pipa does exactly this (TouchProbe.cpp:318, p[i].x * 30480u / xmax). Publishing
// panel px instead makes a 2560-wide panel reach 2560/30480 = 8.4% of the HID range, which is what put every contact of
// every k66 finger into the top-left corner of the screen. The ruler is the unit the consumer reads; the span argument
// is the unit the source emits, and each source states its own.
static int tb_to_ruler(int value, int span, int ruler) {
    if (span < 2 || ruler < 2) return 0;
    if (value < 0) value = 0;
    if (value >= span) value = span - 1;
    return (int)((long long)value * (ruler - 1) / (span - 1));
}

// Touch core output: long axis in x, short in y, both in 0.1 panel px (NVT_SUPER_RESOLUTION).
static int tb_core_x(int x) { return tb_to_ruler(x, NVT_LONG_AXIS_MAX, TB_HID_RULER_LONG); }
static int tb_core_y(int y) {
    if (TB_Y_MIRROR && y >= 0) y = NVT_SHORT_AXIS_MAX - 1 - y;
    return tb_to_ruler(y, NVT_SHORT_AXIS_MAX, TB_HID_RULER_SHORT);
}

void publishRuler(unsigned count, const int *ids, const int *xs, const int *ys) {
    // The scripted link test holds a contact down; the panel's own empty frames (~110/s with no finger on it) would lift
    // it again after every scripted frame, which is how int-283 ended up as 1247 one-frame strokes and no gesture (see the
    // note at swipeThread). Only the scripted publisher sets this flag, so real fingers are never filtered.
    if (gScriptedDown && count == 0) return;
    int rx[NVT_SLOTS], ry[NVT_SLOTS], ri[NVT_SLOTS];
    for (unsigned i = 0; i < count && i < NVT_SLOTS; ++i) { ri[i] = ids[i]; rx[i] = xs[i]; ry[i] = ys[i]; }
    trackStrokes(count < NVT_SLOTS ? count : NVT_SLOTS, ri, rx, ry);
    native_touch_seq = native_touch_seq + 1;
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_count = count;
    for (unsigned i = 0; i < count && i < NVT_SLOTS; ++i) {
        native_touch_xs[i] = rx[i];
        native_touch_ys[i] = ry[i];
        native_touch_ids[i] = ri[i];
    }
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_seq = native_touch_seq + 1;
    void (*notify)(void *) = __atomic_load_n(&gTouchNotify, __ATOMIC_ACQUIRE);
    if (!notify) return;
    native_touch_notify_calls = native_touch_notify_calls + 1;
    notify(gTouchNotifyContext);
}

void publish(unsigned count, const nvt_contact *c) {
    int ids[NVT_SLOTS], xs[NVT_SLOTS], ys[NVT_SLOTS];
    for (unsigned i = 0; i < count && i < NVT_SLOTS; ++i) {
        // The slot index, not the core's tracking id: backboardd drops a path whose index reaches its MAX_PATH_COUNT,
        // and the id counter only ever grows (int-242: ids 200..219 were refused 5088 times, so no swipe ever landed).
        ids[i] = c[i].slot + 1;
        xs[i] = tb_core_x(c[i].x);
        ys[i] = tb_core_y(c[i].y);
    }
    publishRuler(count, ids, xs, ys);
}

// One frame read exactly as this board's own driver does it (novatek_thp.c touch_driver_get_frame): the command byte
// 0x20 in the first tx slot of a single full-duplex transfer, and the reply of that same transfer is the frame, handed
// over untouched. No page select and no event-buffer address is involved - those belong to the other Novatek driver.
// The frame this part's factory library sizes at 3254 bytes (libafehalW2599Z2700.so thp_afe_get_frame: 54-byte header
// plus 16*50 int32 nodes), so the burst is that plus room for the command/dummy bytes this die clocks out in front.
static int burstRead(Session &s) {
    const size_t len = NVT_THP_FRAME_LEN + 8;
    const int rc = nvt_n9z_read_frame(&s.chip, s.frame, len);
    if (rc) {++s.burstRc; return rc;}
    ++s.bursts;
    s.sync = -1;
    // Anchor on the four bytes the part is checked for, not on a crc/inverse pair invented from another die's header:
    // thp_afe_get_frame's only validity test is buf[6..9] == A5 5A 5A A5 (0x15f10-0x15f60), and it is the test that
    // decides whether the frame is used. The burst start is wherever that marker sits six bytes in.
    for (int at = 0; at < 16; ++at) {
        if (!nvt_thp_sync(s.frame + at)) continue;
        ++s.syncHist[at];
        if (s.sync < 0) s.sync = at;
    }
    if (s.sync >= 0) ++s.aligned;
    const uint8_t *hdr = s.frame + (s.sync < 0 ? 0 : s.sync);
    for (unsigned q = 0; q < 24; ++q) s.head[q] = hdr[q];
    return 0;
}

// Decode the burst as the vendor's point report and publish it, because that is the second of the two report formats
// this firmware family streams: the SPI path of nt36xxx_ops.c reads only 153 bytes (POINT_AFT_DATA_LEN) from the same
// command this loop sends and parses nothing but the ten six-byte slots, so a burst that carries no THP header is not
// garbage - it is the other format, and k60..k66 threw every one of them away after counting it.
// Two guards keep a random burst from becoming a finger. The bare slot format is accepted often enough by chance (an id
// is valid in 10 of 32 byte values and an event in 2 of 8) that one believed report is not evidence; a real finger
// survives the ~2 ms between two polls with the same slot id, so a report is published only when an id it carries was
// already present in the previous believed report ("second sight"). And a report that ends publishes an empty set once,
// because the consumer derives tip-up from the finger list going away - without that call a finger would stay down
// forever and the second touch would be refused.
static void pointPass(Session &s) {
    // Once the vendor's own block read answers, it owns the point format: two producers publishing the same six-byte
    // slots from different windows would flap the contact set and the consumer would see no gesture (int-283).
    if (s.prefLive) return;
    struct nvt_point pts[10];
    int start = 0, wdt = 0;
    const int n = nvt_n9z_points(s.frame, NVT_THP_FRAME_LEN + 8, s.spanX, s.spanY, pts, 10, &start, &wdt);
    if (wdt) {
        // nvt_wdt_fw_recovery(): the pattern repeated past recovery_cnt_max(10) means the image reset itself and the
        // vendor re-downloads it. Counted here, and the re-download is the same stage-9 branch the bring-up uses.
        ++s.wdt;
        s.lastPointN = 0;
        if (s.wdt > 10) s.wdt = 0;
        return;
    }
    if (n <= 0) {
        if (s.pointContacts) {
            s.pointContacts = 0;
            static const int none[1] = {0};
            publishRuler(0, none, none, none);   // empty set: the consumer lifts whatever was down
        }
        s.lastPointN = 0;
        return;
    }
    s.pointBase = static_cast<uint32_t>(start);
    int contig = 0;
    for (int a = 0; a < n && !contig; ++a)
        for (int b = 0; b < s.lastPointN && !contig; ++b)
            if (pts[a].id == s.lastPointIds[b]) contig = 1;
    s.lastPointN = n > 10 ? 10 : n;
    for (int a = 0; a < s.lastPointN; ++a) s.lastPointIds[a] = pts[a].id;
    if (!contig) {++s.pointArm; return;}
    int ids[10], xs[10], ys[10];
    for (int a = 0; a < n && a < 10; ++a) {
        ids[a] = pts[a].id;
        // The report's own axes are the devicetree's: x across the 1600-wide short axis, y down the 2560-tall long one
        // (b01.dts:10101-10102, and the vendor bounds-checks input_x against abs_x_max / input_y against abs_y_max).
        // The consumer's ruler is long-then-short, so the long axis is published as x - the same frame the matrix
        // decoder publishes (NvtTouchCore.h:65, "x along the long axis"), which keeps the two branches comparable.
        xs[a] = tb_to_ruler(pts[a].y, s.spanY, TB_HID_RULER_LONG);
        ys[a] = tb_to_ruler(pts[a].x, s.spanX, TB_HID_RULER_SHORT);
    }
    ++s.pointPub;
    s.pointContacts = static_cast<uint32_t>(n);
    native_touch_last_x = pts[0].y;
    native_touch_last_y = pts[0].x;
    native_touch_contacts = static_cast<uint32_t>(n);
    publishRuler(static_cast<unsigned>(n), ids, xs, ys);
}

// Poll the block the vendor's IRQ bottom half actually reads, instead of reinterpreting the 0x20 burst as a point
// report. The vendor's own bounds comment names 0x11E00 ~ 0x11E98 for the AFT report; the other three bases are the
// die-dependent ones our NvtProtocol.h records for this family, so all four are polled every pass (four paged
// 153-byte reads are ~0.5 ms on the wire at the devicetree's 10 MHz, against the 2.6 ms the burst itself takes) and
// the ones that answer are counted - no guess about which layout this die has. Same belief rule as the burst branch:
// an id has to have been present in the previous believed report, so a random SRAM window cannot become a finger.
static const uint32_t kPointBases[4] = {0x11E00u, 0x11A00u, 0x21C00u, 0x22D00u};
#define NVT_POINT_DATA_LEN 153

static void pointBlockPass(Session &s) {
    if (!s.pref) s.pref = static_cast<uint8_t *>(IOMalloc(NVT_POINT_DATA_LEN));
    if (!s.pref) return;
    struct nvt_point pts[10];
    for (unsigned b = 0; b < 4; ++b) {
        if (nvt_read(&s.chip, kPointBases[b], s.pref, NVT_POINT_DATA_LEN)) {++s.prefBusErr; continue;}
        int start = 0, wdt = 0;
        const int n = nvt_n9z_points(s.pref, NVT_POINT_DATA_LEN, s.spanX, s.spanY, pts, 10, &start, &wdt);
        if (wdt) {++s.prefWdt; continue;}
        if (n <= 0) continue;
        ++s.prefSeen[b];
        int contig = 0;
        for (int a = 0; a < n && !contig; ++a)
            for (int q = 0; q < s.prefLastN && !contig; ++q)
                if (pts[a].id == s.prefLastIds[q]) contig = 1;
        s.prefLastN = n > 10 ? 10 : n;
        for (int a = 0; a < s.prefLastN; ++a) s.prefLastIds[a] = pts[a].id;
        if (!contig) {++s.prefArm; continue;}
        int ids[10], xs[10], ys[10];
        for (int a = 0; a < n && a < 10; ++a) {
            ids[a] = pts[a].id;
            xs[a] = tb_to_ruler(pts[a].y, s.spanY, TB_HID_RULER_LONG);
            ys[a] = tb_to_ruler(pts[a].x, s.spanX, TB_HID_RULER_SHORT);
        }
        ++s.prefPub;
        s.prefLive = 1;
        s.prefContacts = static_cast<uint32_t>(n);
        native_touch_last_x = pts[0].y;
        native_touch_last_y = pts[0].x;
        native_touch_contacts = static_cast<uint32_t>(n);
        publishRuler(static_cast<unsigned>(n), ids, xs, ys);
        return;
    }
    s.prefLastN = 0;
    if (s.prefContacts) {
        s.prefContacts = 0;
        static const int none[1] = {0};
        publishRuler(0, none, none, none);
    }
}

// Decode one burst with the official THP matrix decoder, and publish. The replay test is on the payload bytes
// themselves: where the frame counter lives is table-derived inside the factory library (0x15f50 adds two tables
// indexed by the project id) and the one thing certain about it is that a frame the part did not rewrite repeats every
// byte, which is exactly what a stuck contact looks like.
static void burstPass(Session &s) {
    if (s.sync < 0) {
        // No header marker at any offset: this burst is not a THP frame. Try the vendor's other decoder instead of
        // discarding it. slotFrames still counts the older format probe, so a readout can tell the two apart.
        struct nvt_point pts[10];
        int start = 0;
        if (nvt_n9z_report(s.frame, NVT_THP_FRAME_LEN + 8, pts, 10, &start) > 0) ++s.slotFrames;
        pointPass(s);
        return;
    }
    static const unsigned kPayloadOff[4] = {48u, 51u, 54u, 57u};
    const unsigned base = static_cast<unsigned>(s.sync);
    if (!s.payloadOff) {
        // Where the nodes start, and in which of the six layouts, is scored on this panel's real frames: the library
        // anchors both behind a table it never prints, and a wrong choice is not loud - it is only a decoder that
        // never fires. The ranking is the vendor's own acceptance rule (raw_row_delta_max in the caplimit CSV:
        // neighbouring nodes of one row must be close), so an offset or endianness that straddles two different
        // electrodes scores large and loses.
        for (unsigned i = 0; i < 4; ++i) {
            if (base + kPayloadOff[i] + NVT_THP_PAYLOAD_LEN > NVT_THP_FRAME_LEN + 8) {++s.roughSkipped; continue;}
            const uint8_t *payload = s.frame + base + kPayloadOff[i];
            for (unsigned v = 0; v < NVT_THP_V_KINDS; ++v) {
                nvt_thp_nodes(payload, v, s.alt);
                s.roughSum[i][v] += nvt_thp_roughness(s.alt);
            }
            ++s.roughFrames[i];
        }
        if (++s.voted < 24) return;
        int best = -1;
        unsigned bestMean = 0xffffffffu;
        for (unsigned i = 0; i < 4; ++i) {
            if (!s.roughFrames[i]) continue;
            for (unsigned v = 0; v < NVT_THP_V_KINDS; ++v) {
                const unsigned mean = static_cast<unsigned>(s.roughSum[i][v] / s.roughFrames[i]);
                if (mean < bestMean) {bestMean = mean; best = static_cast<int>(i * NVT_THP_V_KINDS + v);}
            }
        }
        if (best < 0) return;
        s.payloadOff = kPayloadOff[static_cast<unsigned>(best) / NVT_THP_V_KINDS];
        s.variant = static_cast<unsigned>(best) % NVT_THP_V_KINDS;
        return;   // the reference must be built from one stable layout, so decoding starts on the next frame
    }
    const uint8_t *payload = s.frame + base + s.payloadOff;
    if (s.haveLast && memcmp(payload, s.lastPayload, NVT_THP_PAYLOAD_LEN) == 0) {++s.replay; return;}
    memcpy(s.lastPayload, payload, NVT_THP_PAYLOAD_LEN);
    s.haveLast = 1;
    s.lastCounter = nvt_thp_counter(s.frame + base);
    ++s.fresh;
    nvt_thp_nodes(payload, s.variant, s.matrix);
    int32_t mn = s.matrix[0], mx = s.matrix[0];
    for (unsigned q = 1; q < NVT_NODES; ++q) {
        if (s.matrix[q] < mn) mn = s.matrix[q];
        if (s.matrix[q] > mx) mx = s.matrix[q];
    }
    s.mn = static_cast<int>(mn);
    s.mx = static_cast<int>(mx);
    // Which way a finger moves this die's raw counts is a property of the image, and the panel's own limits bound
    // noise in both directions (-300..300), so every frame is decoded twice - as it is and negated - and the sign
    // that first produces a contact owns the pass. nvt_touch_process only looks above its threshold
    // (delta_at = matrix - reference), so feeding it the one wrong sign decodes nothing at all, forever.
    struct nvt_contact outA[NVT_SLOTS], outB[NVT_SLOTS];
    const int nA = nvt_touch_process(s.core, s.matrix, 4, outA, NVT_SLOTS);
    for (unsigned q = 0; q < NVT_NODES; ++q) s.alt[q] = -s.matrix[q];
    const int nB = nvt_touch_process(s.coreNeg, s.alt, 4, outB, NVT_SLOTS);
    native_touch_frames = native_touch_frames + 1;
    if (!s.polaritySet) {
        if (nA > 0) s.polaritySet = 1;
        else if (nB > 0) {s.polarity = 1; s.polaritySet = 1;}
        else return;                        // both references still forming, or neither sign has a peak yet
    }
    const struct nvt_contact *out = s.polarity ? outB : outA;
    const int n = s.polarity ? nB : nA;
    if (n < 0) return;                      // the startup reference is still being collected
    native_touch_valid = native_touch_valid + 1;
    native_touch_contacts = static_cast<uint32_t>(n);
    if (n > 0) {
        ++s.matrixPub;
        s.matrixLive = 1;                   // this branch owns the ruler: the point decoders stand down
        native_touch_last_x = out[0].x;
        native_touch_last_y = out[0].y;
    }
    publish(static_cast<unsigned>(n), out);
}

bool snapshot(Session &s) {
    mark(s, 1);
    if (!mapRegion(TB_GCC_BASE + 0x0000, 0x1000, s.gccLow) || !mapRegion(TB_GCC_BASE + 0x18000, 0x1000, s.gccS4) ||
        !mapRegion(TB_GCC_BASE + 0x23000, 0x1000, s.gccHalt) || !mapRegion(TB_GCC_BASE + 0x52000, 0x1000, s.gccVote) ||
        !mapRegion(TB_TLMM_BASE + tb_tlmm_ctl(TB_GPIO_IRQ), 0x1000, s.tlmmIrq) ||
        !mapRegion(TB_TLMM_BASE + tb_tlmm_ctl(TB_GPIO_SPI_MISO), 4 * TB_GPIO_STRIDE, s.tlmmSpi)) {
        fail(1, -12);
        return false;
    }
    uint32_t *v = s.vals;
    v[0] = rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff);
    v[1] = rd32(s.gccHalt.va, TB_HALT_M_AHB & 0xfff);
    v[2] = rd32(s.gccHalt.va, TB_HALT_S_AHB & 0xfff);
    v[3] = rd32(s.gccHalt.va, TB_HALT_CORE & 0xfff);
    v[4] = rd32(s.gccHalt.va, TB_HALT_CORE_2X & 0xfff);
    v[5] = rd32(s.gccS4.va, TB_HALT_S4 & 0xfff);
    v[6] = rd32(s.gccLow.va, TB_GCC_GPLL0_MODE);
    v[7] = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_DFSR_CMD) & 0xfff);
    v[8] = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_CFG) & 0xfff);
    for (unsigned l = 0; l < TB_DFS_LEVELS; ++l) {
        uint32_t cfg = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_PERF_CFG(l)) & 0xfff);
        uint32_t m = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_PERF_M(l)) & 0xfff);
        uint32_t n = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_PERF_N(l)) & 0xfff);
        s.levelHz[l] = tb_dfs_level_hz(cfg, m, n);
        if (l < 4) s.vals[24 + l] = cfg;
    }
    for (unsigned i = 0; i < 4; ++i) v[9 + i] = rd32(s.tlmmSpi.va, i * TB_GPIO_STRIDE);
    v[13] = rd32(s.tlmmIrq.va, 4);
    return true;
}

bool enable(Session &s) {
    mark(s, 2);
    // GPLL0 feeds the DFS levels; vote for it (a shared vote, not an exclusive enable) and wait for lock.
    wr32(s.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff, rd32(s.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff) | 1u);
    for (unsigned i = 0; i < 10000 && !(rd32(s.gccLow.va, TB_GCC_GPLL0_MODE) & (1u << 31)); ++i) IODelay(10);
    s.vals[6] = rd32(s.gccLow.va, TB_GCC_GPLL0_MODE);
    wr32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff, rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff) | TB_VOTE_ALL);
    // Branches with hardware clock gating (bit 1) show CLK_OFF while idle and wake on access, so only a branch
    // without that bit must have CLK_OFF clear.
    auto running = [](uint32_t cbcr) {return !(cbcr & TB_CBCR_CLK_OFF) || (cbcr & 2u);};
    bool ok = false;
    for (unsigned i = 0; i < 2000 && !ok; ++i) {
        ok = running(rd32(s.gccHalt.va, TB_HALT_M_AHB & 0xfff)) && running(rd32(s.gccHalt.va, TB_HALT_S_AHB & 0xfff)) &&
             !(rd32(s.gccHalt.va, TB_HALT_CORE & 0xfff) & TB_CBCR_CLK_OFF) &&
             !(rd32(s.gccHalt.va, TB_HALT_CORE_2X & 0xfff) & TB_CBCR_CLK_OFF) &&
             !(rd32(s.gccS4.va, TB_HALT_S4 & 0xfff) & TB_CBCR_CLK_OFF);
        if (!ok) IODelay(10);
    }
    s.vals[0] = rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff);
    s.vals[1] = rd32(s.gccHalt.va, TB_HALT_M_AHB & 0xfff);
    s.vals[2] = rd32(s.gccHalt.va, TB_HALT_S_AHB & 0xfff);
    s.vals[3] = rd32(s.gccHalt.va, TB_HALT_CORE & 0xfff);
    s.vals[4] = rd32(s.gccHalt.va, TB_HALT_CORE_2X & 0xfff);
    s.vals[5] = rd32(s.gccS4.va, TB_HALT_S4 & 0xfff);
    if (!ok) {fail(2, -110); return false;}   // never touch the engine without running clocks
    const uint32_t cfg = tb_pin_cfg(TB_FUNC_QUP1_SE4, TB_DRIVE_6MA);
    for (unsigned i = 0; i < 4; ++i) wr32(s.tlmmSpi.va, i * TB_GPIO_STRIDE, cfg);
    for (unsigned i = 0; i < 4; ++i) s.vals[9 + i] = rd32(s.tlmmSpi.va, i * TB_GPIO_STRIDE);
    mark(s, 3);   // the next accesses touch the engine: if the page freezes here, the engine access hung
    if (!mapRegion(TB_QUP_WRAPPER_BASE, 0x2000, s.wrapper) || !mapRegion(TB_SE_BASE, TB_SE_SIZE, s.se)) {fail(3, -12); return false;}
    s.spi.se = (struct geni_io){reinterpret_cast<void *>(s.se.va), ioRd, ioWr, ioDelayUs};
    s.spi.wrapper = (struct geni_io){reinterpret_cast<void *>(s.wrapper.va), ioRd, ioWr, ioDelayUs};
    mark(s, 31);   // first engine register read
    int r = geni_spi_read_info(&s.spi);
    s.vals[14] = s.spi.info.qup_hw_version;
    s.vals[15] = s.spi.info.fw_revision;
    s.vals[16] = s.spi.info.if_disable;
    s.vals[17] = s.spi.info.hw_param0;
    if (r) {fail(3, r); return false;}
    return true;
}

bool prepareDma(Session &s) {
    mark(s, 41);
    s.dmaMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
                                                               2 * kDmaBytes, 0xfffffff000ULL);
    if (!s.dmaMem || s.dmaMem->prepare() != kIOReturnSuccess) {fail(41, -12); return false;}
    mark(s, 42);
    addr64_t seg_len = 0;
    s.dmaPhys = s.dmaMem->getPhysicalSegment(0, &seg_len);
    uint8_t *virt = static_cast<uint8_t *>(s.dmaMem->getBytesNoCopy());
    if (!virt || !s.dmaPhys || seg_len < 2 * kDmaBytes) {fail(42, -14); return false;}
    // Cached allocation (kIOMapInhibitCache on RAM above 4 GiB panics in phystokv_range: the cache-attribute path truncates the
    // page number to 32 bits), so the engine's view is made coherent with explicit cache maintenance.
    memset(virt, 0, 2 * kDmaBytes);
    cacheRange(virt, 2 * kDmaBytes, 1);
    if (!mapRegion(TB_APPS_SMMU_BASE, TB_APPS_SMMU_SIZE, s.smmu)) {fail(43, -12); return false;}
    mark(s, 43);
    struct smmu_io io = {reinterpret_cast<void *>(s.smmu.va), smmuRd, smmuWr};
    struct smmu_plan plan = smmu_plan_bypass(&io, TB_QUP_SMMU_SID, s.dmaPhys, 2 * kDmaBytes);
    s.smmuSlot = static_cast<int>(plan.slot); s.smmuBank = static_cast<int>(plan.bank);
    if (plan.error) {s.smmuErr = smmuErrorCode(plan.error); fail(43, -100 - s.smmuErr); return false;}
    const char *e = smmu_configure_bypass(&io, &plan, TB_QUP_SMMU_SID);
    if (e) {s.smmuErr = smmuErrorCode(e); fail(44, -100 - s.smmuErr); return false;}
    s.spi.use_dma = 1;
    s.spi.dma.ctx = nullptr;
    s.spi.dma.clean = dmaClean;
    s.spi.dma.clean_inval = dmaCleanInval;
    s.spi.dma.inval = dmaInval;
    s.spi.dma.tx_virt = virt;
    s.spi.dma.rx_virt = virt + kDmaBytes;
    s.spi.dma.tx_phys = s.dmaPhys;
    s.spi.dma.rx_phys = s.dmaPhys + kDmaBytes;
    s.spi.dma.size = kDmaBytes;
    return true;
}

bool openBus(Session &s, unsigned hz) {
    mark(s, 4);
    s.spi.poll_limit = 400000;
    s.spi.poll_only = 2;             // keep the top-level IRQ enable: the status registers must stay visible to the poll
    // v53-v55 hardware: FIFO_IF_DISABLE=1 and the register that controls it (0x2008) aborts on access, so FIFO transfers
    // cannot be used; the SE DMA engine is the host-visible path.
    if (s.spi.info.fifo_disabled && !prepareDma(s)) return false;
    int r = geni_spi_init(&s.spi);
    if (r) {fail(4, r); return false;}
    (void)hz;
    return true;
}

bool configureClock(Session &s, unsigned hz) {
    struct tb_clock_choice choice = {};
    if (tb_choose_clock(s.levelHz, TB_DFS_LEVELS, s.spi.oversampling, hz, &choice)) {fail(5, -22); return false;}
    s.level = choice.level;
    s.div = choice.div;
    s.actualHz = choice.actual_hz;
    // The divider arithmetic lives in the GENI library; the DFS level is selected through GENI_CLK_SEL.
    int r = geni_spi_set_clock(&s.spi, hz, static_cast<unsigned>(choice.sclk_hz), choice.level);
    if (r) {fail(5, r); return false;}
    r = geni_spi_setup(&s.spi, TB_SPI_MODE, 0);
    if (r) {fail(5, r); return false;}
    return true;
}
}  // namespace

static void touchThread(void *, wait_result_t) {
    Session *s = static_cast<Session *>(IOMallocType(Session));
    if (!s) {fail(0, -12); return;}
    bzero(s, sizeof *s);
    // The point decoder's bounds come from the firmware info block when it answers (nvt_get_fw_info, set below); until
    // then the devicetree's own abs_max_x/abs_max_y are used. spanSrc says which one a readout was decoded with:
    // 1 = devicetree, 2 = firmware info block.
    s->spanX = TB_PANEL_ABS_MAX_X;
    s->spanY = TB_PANEL_ABS_MAX_Y;
    s->spanSrc = 1;
    unsigned stage = native_touch_stage_req, hz = TB_SPI_DEFAULT_HZ;
    PE_parse_boot_argn("sheng-touch-hz", &hz, sizeof hz);
    if (hz < 100000 || hz > TB_SPI_MAX_HZ) hz = TB_SPI_DEFAULT_HZ;
    do {
        if (!snapshot(*s)) break;
        native_touch_stage_done = 1;
        rows(*s);
        if (stage < 2) break;
        if (!enable(*s)) break;
        native_touch_stage_done = 2;
        rows(*s);
        if (stage < 3) break;
        if (!openBus(*s, hz) || !configureClock(*s, hz)) break;
        s->tx = static_cast<uint8_t *>(IOMalloc(NVT_TRANSFER_LEN + 8));
        // The burst read hands the whole reply through the protocol layer's own receive buffer, so it is sized for a
        // frame plus its leading bytes, not for the 256-byte register-read chunk.
        s->rx = static_cast<uint8_t *>(IOMalloc(NVT_FRAME_READ_MAX));
        s->frame = static_cast<uint8_t *>(IOMalloc(NVT_THP_FRAME_LEN + 8));
        s->matrix = static_cast<int32_t *>(IOMalloc(NVT_NODES * sizeof(int32_t)));
        if (!s->tx || !s->rx || !s->frame || !s->matrix) {fail(6, -12); break;}
        s->bus = (struct nvt_bus){s, busXfer, busDelay};
        s->chip = {};
        s->chip.bus = &s->bus;
        s->chip.scratch = s->tx;
        s->chip.rxbuf = s->rx;
        // nvt_identify() picks the register profile from the trim id the part answers with (0x3Fxxx/0x2FExxx for the
        // NT36523 die this board ships), so nothing here pre-selects a map.
        mark(*s, 7);
        // SPI mode: sheng-touch-mode=0..3 forces one; otherwise sweep until the chip id matches (the board DT gives no
        // spi-cpha/spi-cpol, i.e. mode 0, so this is a hardware cross-check rather than a guess).
        // Engine loopback: MOSI is fed back to MISO inside the controller, so this isolates the DMA, packing and clock path
        // from the touch chip.
        {
            static const uint8_t pattern[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00};
            {volatile uint8_t *pre = s->spi.dma.rx_virt; for (unsigned k = 0; k < 64; ++k) pre[k] = 0x5a;}
            geni_spi_setup(&s->spi, GENI_SPI_LOOP, 0);
            s->loopRc = geni_spi_transfer(&s->spi, pattern, s->rx, sizeof pattern, 0);
            for (unsigned k = 0; k < 16; ++k) s->loopRx[k] = s->rx[k];
            s->loopTxIn = s->spi.last_tx_in;
            s->loopRxIn = s->spi.last_rx_in;
            IODelay(2000);   // is the data merely late?
            {volatile uint8_t *late = s->spi.dma.rx_virt; for (unsigned k = 0; k < 8; ++k) s->loopRxLate[k] = late[k];}
            static const uint32_t regs[16] = {SE_DMA_TX_PTR_L, SE_DMA_TX_PTR_H, SE_DMA_TX_ATTR, SE_DMA_TX_LEN, SE_DMA_RX_PTR_L,
                SE_DMA_RX_PTR_H, SE_DMA_RX_ATTR, SE_DMA_RX_LEN, SE_GENI_DMA_MODE_EN, SE_SPI_LOOPBACK, SE_GSI_EVENT_EN, SE_IRQ_EN,
                SE_DMA_TX_IRQ_STAT, SE_DMA_RX_IRQ_STAT, SE_GENI_STATUS, SE_GENI_M_IRQ_STATUS};
            for (unsigned k = 0; k < 16; ++k) s->reg[k] = ioRd(s->spi.se.ctx, regs[k]);
            geni_spi_setup(&s->spi, 0, 0);
            rows(*s);
        }
        unsigned forced = 4;
        PE_parse_boot_argn("sheng-touch-mode", &forced, sizeof forced);
        static const unsigned order[4] = {0, 3, 1, 2};
        int r = NVT_ERR_CHIP;
        for (unsigned i = 0; i < 4 && r; ++i) {
            unsigned m = forced < 4 ? forced : order[i];
            geni_spi_setup(&s->spi, ((m & 1) ? GENI_SPI_CPHA : 0) | ((m & 2) ? GENI_SPI_CPOL : 0), 0);
            s->chip = {};
            s->chip.bus = &s->bus;
            s->chip.scratch = s->tx;
            s->chip.rxbuf = s->rx;
            r = nvt_identify(&s->chip);
            s->modeId[i][0] = s->chip.id[0]; s->modeId[i][1] = s->chip.id[1]; s->modeId[i][2] = s->chip.id[2];
            s->modeId[i][3] = s->chip.id[3]; s->modeId[i][4] = s->chip.id[4]; s->modeId[i][5] = s->chip.id[5];
            s->modeRc[i] = r;
            s->modeTried = i + 1;
            s->modeUsed = m;
            rows(*s);
            if (forced < 4) break;
        }
        if (r) {fail(7, r); rows(*s); break;}
        native_touch_stage_done = 3;
        rows(*s);
        if (stage < 4) break;
        s->core = static_cast<struct nvt_touch_core *>(IOMalloc(sizeof *s->core));
        if (!s->core) {fail(8, -12); break;}
        nvt_touch_reset(s->core);
        s->lastCounter = 0xffffffffu;
        mark(*s, 8);
        // Which half of the official bring-up this part still needs is measured, not assumed. nvt_identify() has just
        // reset the chip into idle (0x69 then 0xAA on the software-reset register, the vendor's own sequence), so the
        // first question is whether an application image streams frames afterwards. Three answers, in the vendor's
        // order: already streaming; streaming after a restart command out of the bootloader (this die boots its own
        // flash image, so no download is involved); or silent - the one case the host download exists for.
        for (unsigned i = 0; i < 8 && s->sync < 0; ++i) {
            if (burstRead(*s)) break;
            IOSleep(10);
        }
        if (s->sync >= 0) s->fwState = 1;
        if (s->sync < 0) {
            // nt36xxx.c sends 0xA5 to restart the image, novatek_thp.c 0xAA; no tree documents which one this die
            // takes, so both are tried and the byte that starts the stream is the one reported.
            static const uint8_t candidates[3] = {0xA5, 0xAA, 0xA5};
            for (unsigned i = 0; i < 3 && s->sync < 0; ++i) {
                if (nvt_fw_restart(&s->chip, candidates[i])) break;
                s->restartByte = candidates[i];
                for (unsigned q = 0; q < 20 && s->sync < 0; ++q) {
                    if (burstRead(*s)) break;
                    IOSleep(10);
                }
                if (s->sync >= 0) s->fwState = 2;
            }
        }
        if (s->sync < 0) {
            mark(*s, 9);
            // Both images the kernel carries are offered to the part, in order. The stock Android tree ships no
            // .bin for this panel at all (huawei_tp.dtsi names one and the firmware partition holds it), so which of
            // the two builds this die accepts cannot be read from a file - only the download handshake answers, and
            // k66 asked once and stopped. fwImage records 1 = primary, 2 = the alternate the link embeds.
            const uint8_t *img[2] = {&native_touch_firmware[0], &native_touch_firmware_alt[0]};
            const uint32_t *szp[2] = {&native_touch_firmware_size, &native_touch_firmware_alt_size};
            for (unsigned idx = 0; idx < 2 && s->sync < 0; ++idx) {
                if (!img[idx] || !szp[idx] || !*szp[idx]) continue;
                ++s->fwTried;
                struct nvt_fw fw;
                s->dlRc = nvt_fw_parse(&fw, img[idx], *szp[idx]);
                if (s->dlRc) continue;
                s->fwParts = fw.count; s->fwCasc = static_cast<unsigned>(fw.cascade_2nd_header);
                s->fwIlm = fw.part[0].size; s->fwDlm = fw.part[1].size;
                s->fwSize = static_cast<unsigned>(*szp[idx]);
                s->fwImage = static_cast<uint8_t>(idx + 1);
                s->chip.reset_retries = 60;   // vendor: 10 polls of 10 ms; longer here to tell "slow" from "never"
                s->dlRc = nvt_download_firmware(&s->chip, &fw);
                if (s->dlRc) continue;
                struct nvt_fw_info info;
                (void)nvt_start_scanning(&s->chip, &info);   // its info block is the other die's layout: recorded, never a gate
                s->vals[18] = info.fw_ver;
                s->vals[19] = info.pid;
                s->vals[20] = info.x_num;
                s->vals[21] = info.y_num;
                s->vals[22] = info.abs_x_max;
                s->vals[23] = info.abs_y_max;
                // The spans the downloaded image publishes are the bounds the vendor parses its point report
                // against, so they replace the devicetree's as soon as they answer (0 < max <= 4096, the same
                // sanity test the public NT36523 driver applies to its info block).
                if (!info.defaulted && info.abs_x_max > 1 && info.abs_y_max > 1 &&
                    info.abs_x_max <= 4096 && info.abs_y_max <= 4096) {
                    s->spanX = info.abs_x_max; s->spanY = info.abs_y_max; s->spanSrc = 2;
                }
                for (unsigned q = 0; q < 50 && s->sync < 0; ++q) {
                    if (burstRead(*s)) break;
                    IOSleep(10);
                }
                if (s->sync >= 0) s->fwState = 3;
            }
            // A failed download is evidence, not a stop: the frame loop is where a finger either lands or does not, and
            // ending the probe here meant the burst branch was never exercised at all.
            if (s->dlRc) fail(9, s->dlRc);
        }
        // An image that was already streaming never went through the path above, so ask it directly for the info block
        // the point report is bounded by. Read-only and never a gate: whatever it answers, the loop below runs.
        if (s->spanSrc == 1) {
            struct nvt_fw_info fi;
            if (!nvt_get_fw_info(&s->chip, &fi) && fi.abs_x_max > 1 && fi.abs_y_max > 1 &&
                fi.abs_x_max <= 4096 && fi.abs_y_max <= 4096) {
                s->spanX = fi.abs_x_max; s->spanY = fi.abs_y_max; s->spanSrc = 2;
            }
            s->vals[22] = fi.abs_x_max;
            s->vals[23] = fi.abs_y_max;
        }
        native_touch_stage_done = 4;
        rows(*s);
        mark(*s, 11);
        unsigned tick = 0;
        // The vendor reads a frame per interrupt; this harness has no GPIO interrupt handler, so the burst is polled.
        // One frame is 3262 bytes at the devicetree's own 10 MHz (~2.6 ms on the wire), and the part produces at ~120 Hz,
        // so a 2 ms pause samples as fast as the panel can fill a frame without tearing one (the counter check drops a
        // replay, the header check drops a torn one).
        for (;;) {
            native_touch_loop_step = 2;
            if ((rd32(s->tlmmIrq.va, 4) & 1) != 0) ++s->irqSeen;
            if (burstRead(*s) == 0) burstPass(*s);
            pointBlockPass(*s);
            native_touch_loop_step = 7;
            if ((++tick & 127) == 0) rows(*s);
            IOSleep(2);
        }
    } while (0);
    rows(*s);
}

// sheng-swipe=<seconds>: our own harness argument, not an official one. The docs stop at "each stage reports on the
// diagnostic page" (docs/drivers/sheng-novatek-touch.md, Probe stages) and define the seqlock publication contract
// (docs/porting/input.md:39, "Publish decoded coordinates, IDs and contact count as one coherent frame"), but they assume a
// working controller. Without one - offline QEMU - the consumer side could never be observed at all, so this publishes a
// few strokes through publishRuler(), the same call the decoder makes, which separates "the OS side is dead" from "the
// panel never published". Coordinates are the core's own units (x along the 25600 long axis, y along the 16000 short one),
// so a stroke lands on the same part of the panel whatever the DTS calls its abs_max. Bounded on purpose: a script that
// keeps touching steals the digitizer paths from a real finger (int-242 refused ids 200..219 5088 times; int-283 left 1247
// one-frame strokes).
extern "C" {volatile uint32_t native_swipe_stats[8];}
static void swipeStroke(int x0, int y0, int x1, int y1, unsigned frames) {
    nvt_contact c = {};
    c.slot = 0; c.peak = 100; c.area = 10; c.weight = 100;
    gScriptedDown = 1;
    for (unsigned f = 0; f <= frames; ++f) {
        c.x = x0 + (x1 - x0) * (int)f / (int)frames;
        c.y = y0 + (y1 - y0) * (int)f / (int)frames;
        publish(1, &c);
        native_swipe_stats[0] = native_swipe_stats[0] + 1;
        IOSleep(8);
    }
    gScriptedDown = 0;
    publish(0, &c);              // the consumer derives tip-up from the set going away
    native_swipe_stats[1] = native_swipe_stats[1] + 1;
}
static void swipeThread(void *, wait_result_t) {
    unsigned delay = 20;
    PE_parse_boot_argn("sheng-swipe", &delay, sizeof delay);
    // Wait for the consumer to exist (NativeHidInput::start installed the sink) and for the UI to be presenting frames,
    // then the configured settle time. A fixed sleep from boot is what made this test unrunnable unattended.
    for (unsigned waited = 0; waited < 2400 && !gTouchNotify; ++waited) IOSleep(500);
    native_swipe_stats[5] = gTouchNotify ? 1 : 0;
    for (unsigned waited = 0; waited < 240 && native_legacy_frame_count < 3 && native_legacy_transfer_calls < 5; ++waited) IOSleep(500);
    native_swipe_stats[6] = native_legacy_frame_count > 255 ? 255 : native_legacy_frame_count;
    native_swipe_stats[4] = 1;
    IOSleep(1000u * delay);
    // Wake the panel first: the lock screen blanks after a few idle seconds and drops touches then (int-232 ignored a whole
    // run). Report 2's Menu bit is the Home press package/HidInputProbe.cpp's tick() sends when the mailbox is set.
    native_hid_input_command = 1;
    for (unsigned i = 0; i < 40 && native_hid_input_command; ++i) IOSleep(100);
    IOSleep(2000);
    native_swipe_stats[2] = touchMs();
    // One pass per axis, in both directions: until a gesture lands, which published axis is the screen's "up" is not known
    // to this build, and a swipe that starts at an edge is the only thing that unlocks (edge::install marks FromEdgeTip).
    swipeStroke(NVT_LONG_AXIS_MAX / 2, NVT_SHORT_AXIS_MAX - 200, NVT_LONG_AXIS_MAX / 2, 1200, 45);
    IOSleep(3000);
    swipeStroke(200, NVT_SHORT_AXIS_MAX / 2, NVT_LONG_AXIS_MAX - 400, NVT_SHORT_AXIS_MAX / 2, 45);
    IOSleep(3000);
    swipeStroke(NVT_LONG_AXIS_MAX / 2, 1200, NVT_LONG_AXIS_MAX / 2, NVT_SHORT_AXIS_MAX - 200, 45);
    native_swipe_stats[3] = touchMs();
}

extern "C" void native_touch_probe(void) {
    unsigned stage = 0, swipe = 0;
    PE_parse_boot_argn("sheng-swipe", &swipe, sizeof swipe);
    if (swipe) {
        thread_t sw = nullptr;
        if (kernel_thread_start(swipeThread, nullptr, &sw) == KERN_SUCCESS) thread_deallocate(sw);
    }
    if (!PE_parse_boot_argn("sheng-touch", &stage, sizeof stage) || stage < 1 || native_touch_stage_req) return;
    if (stage > 4) stage = 4;
    native_touch_stage_req = stage;
    thread_t thread = nullptr;
    if (kernel_thread_start(touchThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_touch_error = -12;
}
