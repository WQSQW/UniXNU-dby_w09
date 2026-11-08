// SPDX-License-Identifier: BSD-2-Clause-Patent
// The audio path of this board: the legacy (pre-AudioReach) packet router apr_audio_svc, the Primary TDM speaker port and one PCM stream
// (boot argument dby-audio=1 or 2, needs sheng-adsp=2; dby-apr picks how far the ladder goes, default 3). The four TFA9874 amplifiers already
// answer and are initialised by DbySmartPA.cpp; what was missing is the DSP side that feeds them samples.
//
// Mechanics come from the official SM8250 board's audio half (upstream/alioth package/AudioProbe.cpp): one work loop, the DSP thread's
// callbacks only copy a message into a small ring and signal an IOInterruptEventSource, and two IOTimerEventSources drive the retry and
// answer pauses, with a sixteen-word stats table for the console. The protocol and every constant come from this unit's own vendor tree
// (techpack/audio: apr.h, q6core.c, q6afe-v2.c, q6adm.c, q6asm.c; arch/arm64/boot/dts/vendor/qcom/kona* and
// kona/DBY/W09_VA/huawei-audio.dtsi); alioth queries the AudioReach framework manager over GPR and has no playback ladder at all, so the
// substitution is the documented next step of its own header, not a new design. ShengApr.c encodes and decodes; this file only owns the
// channel, the timers and the order.
//
// dby-apr levels: 1 channel + AVCS handshake, 2 + TDM pad mux, AFE clock/config/start and the ADM route, 3 + one mapped PCM stream that
// plays a tone (the audible proof that the whole path carries samples).
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
extern "C" {
#include "ShengAdsp.h"
#include "ShengApr.h"
#include "SmmuBypass.h"
#include "TouchBoard.h"
}
#include "ShengAudioAbi.h"

// The service iOS's audio server plug-in opens (package/ShengAudioService.cpp, interface ShengAudioAbi.h). Its class registration is
// taken out of __mod_init_func by the build and called here by name between preModLoad and postModLoad, the same staging DbyBattery.cpp
// and AppleARMBacklight.cpp use.
extern "C" void native_audio_service_static_init(void) __asm__("__GLOBAL__sub_I_ShengAudioService.cpp");
extern "C" void sheng_audio_service_start(void);
// [0] service state (1 published, negative the step that failed), [1] opens, [2] closes, [3] requests, [4] refused requests,
// [5] memory maps, [7] the loudest sample the pump took from the ring (low half) and from capture (high half), x 32767.
extern "C" {extern volatile int32_t native_audio_service[8];}

extern "C" ShengAdspChannel *sheng_adsp_channel(const char *, uint32_t, uint32_t, int, const ShengAdspClient *, void *) __attribute__((weak_import));
extern "C" int sheng_adsp_send(ShengAdspChannel *, const void *, uint32_t) __attribute__((weak_import));
extern kmod_info_t g_kernel_kmod_info;   // the module the service's classes register against
// package/DbySmartPA.cpp: the factory amplifier on-scene (DbyPaTables.h's kDbyPaTfa9874_*_on[]), which the vendor applies only
// after every device has started -- INIT alone leaves register 0x00 at 0x0002, so AMPE and DCA stay clear and nothing can be heard.
extern "C" uint32_t dby_smartpa_play(void);
// The cache clean the ADSP probe already uses for every buffer it hands the DSP (package/AdspProbe.cpp:296). That copy has
// internal linkage (it sits inside the anonymous namespace opened at AdspProbe.cpp:119), so this translation unit carries its
// own identical copy instead of an external reference; clang drops the out-of-line body in AdspProbe.o once it inlines it.
static void cleanToPoc(const void *start, size_t length) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(start) & ~(line - 1); a < reinterpret_cast<uintptr_t>(start) + length; a += line)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// [0] step reached (positive) or the step that failed (negative), [1] requests sent, [2] answers, [3] last ADSP status,
// [4] refusals (a basic result with a non-zero status), [5] answer timeouts, [6] packets that matched no request,
// [7] ms from channel up to the last step reached, [8] channel ups, [9] channel downs, [10] last opcode received,
// [11] messages dropped (ring full), [12] send refusals by the transport, [13] dby-apr level, [14] write-done events,
// [15] the memory-map handle the DSP gave back.
extern "C" {volatile int32_t native_audio_stats[16];}
// The GLINK link our channel rides on, published by the official ADSP code: [9] messages in, [10] out, [11] the channels that are up
// (one bit per client) | version agreed << 14 | link broken << 15, [12] opens nobody serves.
extern "C" {extern volatile int32_t native_adsp_stats[16];}
extern "C" {extern volatile int32_t native_adsp_audio[14];}

namespace {
constexpr uint32_t kAnswerMs = 1000, kRetryMs = 100, kTries = 3;   // Q6_READY_TIMEOUT_MS and the pause between two tries
constexpr uint32_t kRing = 8, kMessage = APR_MAX_PACKET;
constexpr uint32_t kIntentSize = 0x200, kIntentCount = 20;         // kona.dtsi:4191-4216, qcom,intents = <0x200 20>

// ---- this unit's speaker backend, from kona/DBY/W09_VA/huawei-audio.dtsi and asoc/kona.c ----
constexpr uint16_t kTdmPort = AFE_PORT_ID_PRIMARY_TDM_RX;           // 0x9000, the four-slot port the DBY backend uses
constexpr uint32_t kRate = 48000, kBits = 16, kChannels = 4;        // 4 slots x 32 bit at 6144000 Hz, 16-bit samples
constexpr uint32_t kClockHz = AFE_TDM_CLK_HZ;                       // 6144000
constexpr uint32_t kPeriodFrames = 1024;                            // one region: 4 ch x 2 B x 1024 frames
constexpr uint32_t kRegionBytes = kPeriodFrames * kChannels * (kBits / 8);   // 8192
constexpr uint32_t kRegions = 4;                                    // four regions in flight = about 85 ms of audio
constexpr uint8_t kSession = 1, kStream = ASM_STREAM_ID_1;          // the ports of every ASM command name this pair

// The LPASS audio master's stream id, straight out of this unit's own heap node: kona-audio.dtsi:5 is
// `iommus = <&apps_smmu 0x1801 0x0>`, and the second cell is the stream-match mask (arm-smmu.c:4020 pulls it back out of
// fwspec->ids[i] >> SMR_MASK_SHIFT), so the entry Linux writes for this device is an exact match with mask 0.
constexpr uint16_t kLpassSid = 0x1801;
// The next line of that node, `qcom,smmu-sid-mask = /bits/ 64 <0xf>`, is not that mask. msm_audio_ion.c:757 reads the property,
// :773 ANDs it with the first iommus cell (0x1801 & 0xf = 1) and :776 shifts the result up by MSM_AUDIO_SMMU_SID_OFFSET, which is
// 32 (:28). Every buffer address the heap then hands out carries that tag: :203 and :437 OR it in, and a packet splits the 64-bit
// value into the two fields the DSP reads - lower_32_bits plus msm_audio_populate_upper_32_bits, which on a 64-bit dma_addr_t is
// simply upper_32_bits (:681-687). q6asm_memory_map does it at :8272-8273 for the map and q6asm_write at :9508-9509 for every
// buffer, so the ADSP is told "stream 1, address pa" and the SMMU entry that answers it is the one keyed on 0x1801.
constexpr uint64_t kSidTag = (uint64_t)(kLpassSid & 0xf) << 32;

// ---- the half the audio server sees (ShengAudioAbi.h, service package/ShengAudioService.cpp) ----
// The plug-in mixes two channels into a 65536-frame ring in 480-frame (10 ms) IO cycles; this backend moves 1024-frame regions through a
// four-slot TDM port, so one region is taken as 1024 frames of the shared timeline and up-mixed at the moment it is handed to the DSP.
// The two clocks are the same crystal: the TDM bit clock is 6144000 Hz = 48000 * 4 slots * 32 bit, which the ADSP divides from the same
// XO that mach_absolute_time counts on, so the ring position and the host timeline need no resync between them (upstream's producer needs
// one because its AudioReach port is clocked from a separate MCLK).
constexpr uint32_t kRingFrames = SHENG_AUDIO_RING_FRAMES;         // 65536 frames, 1.37 s
constexpr uint32_t kAbiOutChannels = SHENG_AUDIO_OUT_CHANNELS;    // 2 from the audio server, 4 slots to the TDM port

// One period of a sine, sampled at 48 kHz with 64 samples per period: 750 Hz exactly, no drift across regions because the index
// keeps counting. The values are round(8000 * sin(2*pi*i/64)); the peak stays well under the 32767 that a 16-bit sample holds.
constexpr uint32_t kToneSteps = 64;
constexpr int16_t kTone[kToneSteps] = {
    0, 784, 1561, 2322, 3061, 3771, 4445, 5075, 5657, 6184, 6652, 7055, 7391, 7656, 7846, 7961,
    8000, 7961, 7846, 7656, 7391, 7055, 6652, 6184, 5657, 5075, 4445, 3771, 3061, 2322, 1561, 784,
    0, -784, -1561, -2322, -3061, -3771, -4445, -5075, -5657, -6184, -6652, -7055, -7391, -7656, -7846, -7961,
    -8000, -7961, -7846, -7656, -7391, -7055, -6652, -6184, -5657, -5075, -4445, -3771, -3061, -2322, -1561, -784,
};

enum {
    StStarted = 1, StChannel = 2, StUp = 3, StVersion = 4, StCore = 5, StPads = 6, StClock = 7, StTdm = 8, StSlots = 9, StPort = 10,
    StCopp = 11, StMatrix = 12, StOpen = 13, StMapped = 14, StFormat = 15, StRun = 16, StPlaying = 17,
    // Appended, so every number already quoted from an earlier round keeps its meaning.
    StVolume = 18,
};

struct Audio {
    IOWorkLoop *loop;
    IOTimerEventSource *answer, *retry, *pace;
    IOInterruptEventSource *inbox;
    IOSimpleLock *ringLock;
    struct {uint32_t length; uint8_t data[kMessage];} ring[kRing];
    uint32_t ringHead, ringCount;
    volatile uint32_t up, changed;
    bool wasUp;
    uint64_t upAt;
    unsigned level;
    AprPending pending;
    ShengAdspChannel *channel;
    int step;              // the ladder entry being waited for, -1 when none is
    uint32_t token;        // the token the waiting request was sent with
    uint32_t tries;
    // the PCM stream
    IOBufferMemoryDescriptor *pcm;
    uint8_t *pcmVa;
    uint64_t pcmPa;
    uint32_t memHandle;
    uint16_t coppId;       // what the ADM open answer reported for the TDM COPP; the matrix map routes to this, not to a port id
    uint32_t busyMask;     // one bit per region while the DSP still holds it
    uint32_t toneIndex, filled, completed;
    uint32_t afeApi, admApi, asmApi;                           // what the AVCS version answer reported
    uint16_t volumeQ13;                                        // the ASM soft volume's linear Q13 master gain
    // the control page and the two rings the audio server's plug-in maps (memory types 0, 1 and 2)
    IOBufferMemoryDescriptor *control, *outRing, *inRing;
    ShengAudioControl *ctl;
    int16_t *out;                                              // kRingFrames frames of kAbiOutChannels 16-bit samples
    volatile uint32_t serving;                                 // 1 once the service is published and the rings may be handed out
    volatile uint32_t want;                                    // the plug-in's start and stop requests, as ShengAudio*Running bits
    uint64_t issued;                                           // regions handed to the DSP: the timeline frame is issued * kPeriodFrames
    uint32_t volumeMilli, volumeSets;                          // the slider the plug-in recorded, and how often the audio server moved it
    uint32_t late, holes;                                      // regions taken before the plug-in wrote them, and those that held a 64-frame run of silence
    uint32_t lastRow;                                          // what the last [apr] os= line said, so the row only appears when it changes
};
Audio gAudio;

void bump(int index) {native_audio_stats[index] = native_audio_stats[index] + 1;}
// Host milliseconds since boot. The request trail keeps this, so a row of native_audio_requests says both when and what.
uint64_t nowMs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000;
}
void record() {
    native_audio_stats[1] = (int32_t)gAudio.pending.sent;
    native_audio_stats[2] = (int32_t)gAudio.pending.answers;
    native_audio_stats[3] = (int32_t)gAudio.pending.last_status;
    native_audio_stats[4] = (int32_t)gAudio.pending.errors;
    native_audio_stats[5] = (int32_t)gAudio.pending.timeouts;
    native_audio_stats[6] = (int32_t)gAudio.pending.unmatched;
    native_audio_stats[10] = (int32_t)gAudio.pending.last_opcode;
    native_audio_stats[14] = (int32_t)gAudio.completed;
    native_audio_stats[15] = (int32_t)gAudio.memHandle;
    // The timeline: frames the DSP has taken back, which is what the plug-in compares its own write position against.
    if (gAudio.ctl) gAudio.ctl->out_frames = gAudio.completed * kPeriodFrames;
}
// The ladder says which step it reached (positive) or died on (negative), and how long the DSP took to get there.
void reached(int step) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - gAudio.upAt, &ns);
    native_audio_stats[7] = (int32_t)(ns / 1000000);
    if (native_audio_stats[0] != step) kprintf("[apr] step=%d ms=%d status=%d op=%x\n", step, native_audio_stats[7],
                                               native_audio_stats[3], native_audio_stats[10]);
    native_audio_stats[0] = step;
}
// The transport verdict belongs in the same row as the ladder's: a rung that gets nothing back is only worth reading once we know
// whether the packet left us at all (tx), whether the link ever came up (link) and whether an answer arrived that we did not match (none).
// cid=/ack= are the two halves of the open dance on our own channel -- an ack with no cid is a channel that never learned the DSP's number,
// a cid with no ack is one the DSP never confirmed -- and unmatch= counts the DSP messages keyed by a number we do not have.
void failed(int step) {
    native_audio_stats[0] = -step;
    record();
    kprintf("[apr] fail step=%d status=%d op=%x tries=%d tx=%d rx=%d link=%x opens=%d sent=%d ans=%d none=%d ref=%d "
            "glink=%d:%d:%d cid=%d ack=%d rop=%d unmatch=%d/%x,%d\n", step,
            native_audio_stats[3], native_audio_stats[10], gAudio.tries, native_adsp_stats[10], native_adsp_stats[9],
            native_adsp_stats[11], native_adsp_stats[12], native_audio_stats[1], native_audio_stats[2],
            native_audio_stats[6], native_audio_stats[12],
            native_adsp_audio[0], native_adsp_audio[1], native_adsp_audio[2],
            native_adsp_audio[9], native_adsp_audio[10], native_adsp_audio[11],
            native_adsp_audio[12], native_adsp_audio[13] >> 16, native_adsp_audio[13] & 0xffff);
}

// ---- the TDM pads (huawei-audio.dtsi:44-46 -> kona-pinctrl.dtsi:1858-1960, pinctrl-kona.c:1694-1700) ----
// gpio138 mi2s0_sck, gpio139 mi2s0_data0, gpio140 mi2s0_data1, gpio141 mi2s0_ws: all WEST, function slot 1, drive-strength 8 mA,
// bias-disable and output-high. pinctrl-msm does this as three separate writes -- the mux field alone, then the drive and pull fields,
// then the output latch in the io register -- and so do we.
// The field layout is the one TouchBoard.h's own tb_pin_cfg() writes for the touch pins (function at bit 2, drive at bit 6, pull
// disabled at bits 0-1); it is spelled out here instead of added to that header because the touch driver that already works on the
// board includes it, and a pad the panel never touches has no business changing shared arithmetic.
constexpr uint32_t kMuxBit = 2, kPullBit = 0, kDrvBit = 6, kDrive8mA = 3;   // (mA/2)-1, the same code tb_pin_cfg uses
constexpr uint32_t kWestBase = 0x00100000;                                  // pinctrl-kona.c tlmm_data.west_base
uint32_t padCtl(unsigned gpio) {return kWestBase + 0x1000u * gpio;}          // CMNVC(pin): tile base plus 0x1000 per pin
bool mapRegion(uint64_t base, uint32_t size, IOMemoryMap *&out, volatile uint8_t *&va, IOOptionBits cache = kIOMapInhibitCache) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    out = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    if (memory) memory->release();
    va = out ? reinterpret_cast<volatile uint8_t *>(out->getVirtualAddress()) : nullptr;
    return va != nullptr;
}
volatile uint8_t *gTlmm;
IOMemoryMap *gTlmmMap;
bool muxPads() {
    static const uint32_t kPads[] = {138, 139, 140, 141};
    const uint32_t muxMask = 0xfu << kMuxBit;   // GENMASK(mux_bit + order_base_2(nfuncs) - 1, mux_bit), nfuncs 10
    if (!gTlmm) return false;
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t ctl = padCtl(kPads[i]);
        uint32_t value = *reinterpret_cast<volatile uint32_t *>(gTlmm + ctl);
        value = (value & ~muxMask) | (1u << kMuxBit);                         // function slot 1
        *reinterpret_cast<volatile uint32_t *>(gTlmm + ctl) = value;
        value &= ~((3u << kPullBit) | (7u << kDrvBit));
        value |= kDrive8mA << kDrvBit;                                        // 8 mA, bias-disable
        *reinterpret_cast<volatile uint32_t *>(gTlmm + ctl) = value;
        *reinterpret_cast<volatile uint32_t *>(gTlmm + ctl + 4) |= TB_PIN_HIGH;  // output-high sets the latch bit only
        if ((*reinterpret_cast<volatile uint32_t *>(gTlmm + ctl) & muxMask) != (1u << kMuxBit)) return false;
    }
    return true;
}

// ---- the ladder ----
struct Step {
    int step;                       // the stage reached when this one is answered
    uint32_t request, response;     // what apr_pending_add matches on
    uint32_t (*encode)(uint8_t *out, uint32_t capacity, uint32_t token);
    int level;                      // the dby-apr level that runs this step
};
uint32_t encVersion(uint8_t *out, uint32_t capacity, uint32_t) {return apr_avcs_get_fwk_version(out, capacity);}
uint32_t encCore(uint8_t *out, uint32_t capacity, uint32_t token) {return apr_core_get_state(out, capacity, token);}
uint32_t encClock(uint8_t *out, uint32_t capacity, uint32_t) {
    const AprClkSet clk = {AFE_CLK_ID_PRI_TDM_IBIT, kClockHz, 1, AFE_CLK_ATTR_INVERT_COUPLE_NO, AFE_CLK_ROOT_DEFAULT};
    return apr_afe_clock_set(out, capacity, &clk);            // always sent with token AFE_CLK_TOKEN
}
uint32_t encTdm(uint8_t *out, uint32_t capacity, uint32_t token) {
    const AprTdmCfg cfg = {kChannels, kRate, kBits, AFE_LINEAR_PCM_DATA, AFE_PORT_TDM_SHORT_SYNC_SLOT_MODE,
                           AFE_PORT_TDM_SYNC_SRC_INTERNAL, 4, 0, 1, 1, 32, 0xF};
    return apr_afe_tdm_config(out, capacity, token, kTdmPort, &cfg);
}
uint32_t encSlots(uint8_t *out, uint32_t capacity, uint32_t token) {
    // The offsets are byte offsets into the frame, not slot indexes: kona.c:2044 sets max_slot_offset = TDM_SLOT_WIDTH_BYTES *
    // (tdm_max_slots - 1) and :2049-2051 rejects anything that is not a multiple of 4 ("Offset value can only be 0, 4, 8, .."), and
    // msm-dai-q6-v2.c:9540 turns the byte offset back into a slot index with slot_offset[i] / slot_width_bytes. So the four channels
    // of this board's 4-slot, 32-bit PRI TDM (huawei-audio.dtsi, qcom,tdm-max-slots = <4>) go to 0, 4, 8, 12. Passed as 0, 1, 2, 3 the
    // DSP lands every channel inside slot 0 and three of the four TFA9874s are fed nothing.
    static const uint16_t kOff[] = {0, 4, 8, 12};
    const AprSlotMap map = {kChannels, kBits, 0, gAudio.afeApi >= AFE_API_VERSION_V3, kOff};
    return apr_afe_slot_mapping(out, capacity, token, kTdmPort, &map);
}
uint32_t encPort(uint8_t *out, uint32_t capacity, uint32_t token) {return apr_afe_port_start(out, capacity, token, kTdmPort);}
uint32_t encCopp(uint8_t *out, uint32_t capacity, uint32_t token) {
    const AprAdmOpen o = {kTdmPort, ADM_PATH_PLAYBACK, kChannels, kBits, ADM_TOPOLOGY_NULL, kRate};
    return apr_adm_device_open(out, capacity, token, &o);
}
uint32_t encMatrix(uint8_t *out, uint32_t capacity, uint32_t token) {
    // The route binds the ASM session to the COPP the ADM handed back when the device was opened (q6adm.c:1854-1857 stores it,
    // :3722-3735 sends it). A port id here is a different number and the ADSP refuses it.
    if (!gAudio.coppId || gAudio.coppId == ADM_INVALID_COPP_ID) return 0;
    return apr_adm_matrix_map(out, capacity, token, kSession, &gAudio.coppId, 1);
}
uint32_t encOpen(uint8_t *out, uint32_t capacity, uint32_t token) {
    return apr_asm_open_write(out, capacity, token, kSession, kStream, kBits, ASM_POPP_TOPOLOGY_DEFAULT);
}
uint32_t encMap(uint8_t *out, uint32_t capacity, uint32_t token) {
    uint64_t addr[kRegions];
    uint32_t size[kRegions];
    // Each region is one period of the ring and the pool is the 4 KB one, so the size the DSP is told has to be the real
    // 8192-byte stride (struct avs_shared_map_region_payload's mem_size_bytes, q6asm.c:8274).
    for (uint32_t i = 0; i < kRegions; ++i) {
        addr[i] = (gAudio.pcmPa + i * kRegionBytes) | kSidTag;    // q6asm.c:8273 puts the tag in shm_addr_msw
        size[i] = kRegionBytes;
    }
    return apr_asm_memory_map(out, capacity, token, addr, size, kRegions);
}
uint32_t encFormat(uint8_t *out, uint32_t capacity, uint32_t token) {
    // mode 0 is the default Q format for the word size, the endianness field is written by the encoder itself
    const AprPcmFormat f = {kChannels, kBits, kBits, 0, kRate};
    return apr_asm_media_format(out, capacity, token, kSession, kStream, &f);
}
uint32_t encRun(uint8_t *out, uint32_t capacity, uint32_t token) {
    return apr_asm_run(out, capacity, token, kSession, kStream, 0, 0);   // flags 0 = run immediately
}
// The session's loudness. This legacy path has no hardware volume register to write: the four TFA9874s hold the gain their profile
// puts in register 0x52 and the vendor driver never touches that field at runtime (DbyPaTables.h writes it once, tfa98xx_genregs
// _N1C.h:3351), so q6asm_set_volume's in-band soft volume is where this ADSP sets the level (dsp/q6asm.c:8803, :8836-8842).
uint32_t encVolume(uint8_t *out, uint32_t capacity, uint32_t token) {
    return apr_asm_set_volume(out, capacity, token, kSession, kStream, gAudio.volumeQ13);
}
const Step kLadder[] = {
    {StVersion, APR_OP_AVCSS_GET_FWK_VERSION, APR_OP_AVCSS_GET_FWK_VERSION_RSP, encVersion, 1},
    {StCore,    APR_OP_CORE_GET_STATE,        APR_OP_CORE_STATE_RSP,            encCore,    1},
    {StClock,   APR_OP_AFE_SVC_SET_PARAM,     APR_OP_BASIC_RESULT,              encClock,   2},
    {StTdm,     APR_OP_AFE_PORT_SET_PARAM_V2, APR_OP_BASIC_RESULT,              encTdm,     2},
    {StSlots,   APR_OP_AFE_PORT_SET_PARAM_V2, APR_OP_BASIC_RESULT,              encSlots,   2},
    {StPort,    APR_OP_AFE_PORT_START,        APR_OP_BASIC_RESULT,              encPort,    2},
    {StCopp,    APR_OP_ADM_DEVICE_OPEN_V8,    APR_OP_ADM_DEVICE_OPEN_RSP_V8,    encCopp,    2},
    {StMatrix,  APR_OP_ADM_MATRIX_MAP_V5,     APR_OP_BASIC_RESULT,              encMatrix,  2},
    {StOpen,    APR_OP_ASM_OPEN_WRITE_V3,     APR_OP_BASIC_RESULT,              encOpen,    3},
    {StMapped,  APR_OP_ASM_MEM_MAP,           APR_OP_ASM_MEM_MAP_RSP,           encMap,     3},
    {StFormat,  APR_OP_ASM_MEDIA_FMT_V2,      APR_OP_BASIC_RESULT,              encFormat,  3},
    // Before the run, so the gain is in place when the first samples reach the block. The encoder answers with the V3 opcode if the
    // ADSP ever reports that it carries instance ids; nothing on this board sets that flag, so V2 is what goes out.
    {StVolume,  APR_OP_ASM_SET_PP_PARAMS_V2,  APR_OP_BASIC_RESULT,              encVolume,  3},
    {StRun,     APR_OP_ASM_RUN_V2,            APR_OP_BASIC_RESULT,              encRun,     3},
};
constexpr uint32_t kSteps = sizeof kLadder / sizeof kLadder[0];

// ---- the half the audio server sees (ShengAudioAbi.h; the service itself is package/ShengAudioService.cpp) ----
// Three shared objects: the 4 KiB control page (ShengAudioControl, then ShengAudioDiag at 2048 and ShengAudioTrace at 2560) and the two
// sample rings. They are allocated for the kernel task with kIOMemoryKernelUserShared, which is what lets the user client hand them to
// the audio server's task without a copy, and they are held for the whole boot: the anchors go up once, so one frame counter names the
// same moment in the kernel and in the audio server for the life of the service.
bool setupShared() {
    const IOOptionBits shared = static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryKernelUserShared);
    gAudio.control = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared, 4096, 4096);
    gAudio.outRing = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared, kRingFrames * kAbiOutChannels * 2, 4096);
    gAudio.inRing = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared, kRingFrames * SHENG_AUDIO_IN_CHANNELS * 2, 4096);
    if (!gAudio.control || !gAudio.outRing || !gAudio.inRing) return false;
    gAudio.ctl = static_cast<ShengAudioControl *>(gAudio.control->getBytesNoCopy());
    gAudio.out = static_cast<int16_t *>(gAudio.outRing->getBytesNoCopy());
    memset(gAudio.ctl, 0, 4096);
    memset(gAudio.out, 0, kRingFrames * kAbiOutChannels * 2);
    memset(gAudio.inRing->getBytesNoCopy(), 0, kRingFrames * SHENG_AUDIO_IN_CHANNELS * 2);
    mach_timebase_info_data_t tb;
    clock_timebase_info(&tb);
    ShengAudioControl *c = gAudio.ctl;
    c->rate = kRate; c->out_channels = SHENG_AUDIO_OUT_CHANNELS; c->in_channels = SHENG_AUDIO_IN_CHANNELS;
    c->period_frames = SHENG_AUDIO_PERIOD_FRAMES; c->ring_frames = kRingFrames;
    c->lead_frames = 30 * SHENG_AUDIO_PERIOD_FRAMES;                    // the 300 ms the plug-in has to stay ahead of the play position
    // Host ticks per frame in 32.32 fixed point: 1e9 * denom / (rate * numer). The kit's producer writes it with a 128-bit divide;
    // this image links no compiler_rt (___udivti3), so the whole and fractional ticks are divided apart here. The remainder is below
    // the divisor and the divisor is 3e7 on this board, so rem << 32 stays inside 64 bits and the answer is the same 400.00000000
    // host ticks for one frame of 48 kHz sound on the 19.2 MHz timer (timebase 625/12) that this board's device tree declares.
    const uint64_t num = 1000000000ULL * tb.denom, den = (uint64_t)kRate * tb.numer;
    if (!den || num / den > 0xffffffffu) return false;
    const uint64_t whole = num / den, rem = num - whole * den;
    c->ticks_per_frame_q32 = (whole << 32) + (rem << 32) / den;
    c->out_anchor_host = mach_absolute_time();
    c->in_anchor_host = c->out_anchor_host;
    c->version = SHENG_AUDIO_VERSION;
    __atomic_store_n(&c->magic, SHENG_AUDIO_MAGIC, __ATOMIC_RELEASE);
    return true;
}

// What the plug-in recorded about the audio server, in the second half of the control page; null before the page exists.
const ShengAudioDiag *diag() {
    return gAudio.ctl ? reinterpret_cast<const ShengAudioDiag *>(reinterpret_cast<const uint8_t *>(gAudio.ctl) +
                                                                   SHENG_AUDIO_DIAG_OFFSET) : nullptr;
}

// The loudest playback sample the pump has taken out of the ring, x 32767, in the low half of [audio-service] peak=. The high half is
// capture, which this board's ladder does not run yet, so nothing writes it.
void notePeak(int32_t peak) {
    if (peak > 0x7fff) peak = 0x7fff;
    const uint32_t now = (uint32_t)native_audio_service[7], mine = now & 0xffffu;
    if ((uint32_t)peak > mine) native_audio_service[7] = (int32_t)((now & ~0xffffu) | (uint32_t)peak);
}

// Takes the kPeriodFrames timeline frames that begin at ring frame `start` and lays them over the port's four slots: sd0 carries slots 0
// and 1 and sd1 slots 2 and 3 (huawei-audio.dtsi's pri_mi2s_sd0/sd1 pinctrl with qcom,tdm-max-slots = 4), so the stereo pair repeats per
// data line and neither channel is lost whichever edge of the tablet each line drives. Every frame is cleared once taken, so a period
// the audio server had not written yet plays as silence instead of repeating what the ring held one turn earlier.
void takeRing(int16_t *dst, uint64_t start) {
    uint32_t at = (uint32_t)(start % kRingFrames);
    int32_t peak = 0;
    for (uint32_t i = 0; i < kPeriodFrames; ++i) {
        const int16_t left = gAudio.out[at * kAbiOutChannels], right = gAudio.out[at * kAbiOutChannels + 1];
        dst[i * kChannels + 0] = left;   dst[i * kChannels + 1] = right;
        dst[i * kChannels + 2] = left;   dst[i * kChannels + 3] = right;
        gAudio.out[at * kAbiOutChannels] = 0;   gAudio.out[at * kAbiOutChannels + 1] = 0;
        const int32_t a = left < 0 ? -left : left, b = right < 0 ? -right : right;
        if (a > peak) peak = a;
        if (b > peak) peak = b;
        at = at + 1 == kRingFrames ? 0 : at + 1;
    }
    notePeak(peak);
}

// Late: the stream was running and the audio server had not written these frames yet, within the 100 ms after its last write (the first
// rounds before its first IO cycle do not count). A hole: frames it had written past, but 64 silent frames in a row inside a region that
// also carries sound. Both are the audio server's own timing, not this ladder's, and they are what says so.
void watchRing(uint64_t start, const int16_t *dst) {
    ShengAudioControl *c = gAudio.ctl;
    uint64_t recent = 0;
    nanoseconds_to_absolutetime(100000000ULL, &recent);
    if (c->out_written < start + kPeriodFrames && mach_absolute_time() - c->out_written_host < recent) {
        gAudio.late++;
        return;
    }
    if (c->out_written < start + kPeriodFrames) return;                       // long stopped: silence is what was asked for
    uint32_t run = 0, longest = 0, loud = 0;
    for (uint32_t i = 0; i < kPeriodFrames; ++i) {
        const int16_t *f = dst + (uint64_t)i * kChannels;
        const bool quiet = !f[0] && !f[1] && !f[2] && !f[3];
        run = quiet ? run + 1 : 0;
        if (run > longest) longest = run;
        if (!quiet) loud++;
    }
    if (longest >= 64 && loud) gAudio.holes++;
}

// Registers the service's classes with the kernel module and starts the service, the same staging DbyBattery.cpp and
// AppleARMBacklight.cpp use: leaving the initializer in __mod_init_func would register the classes a second time, and this tree has
// already panicked once on a duplicate OSMetaClass registration.
bool publishService() {
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return false;
    native_audio_service_static_init();
    if (OSMetaClass::postModLoad(handle)) return false;
    sheng_audio_service_start();
    return true;
}

// [apr] os=: the audio server's side, once per change. volume= is the slider the plug-in holds (scalar x 1000): the plug-in scales every
// sample before it writes the ring, so the kernel's own ASM soft volume stays at unity and the two never attenuate twice.
void serviceRow() {
    const ShengAudioDiag *d = diag();
    if (!d || !gAudio.serving) return;
    gAudio.volumeMilli = d->volume_milli;
    gAudio.volumeSets = d->volume_sets;
    const uint32_t key = (gAudio.volumeMilli << 4) | (gAudio.want & 15u);
    if (key == gAudio.lastRow) return;
    gAudio.lastRow = key;
    kprintf("[apr] os=%u want=%u sets=%u opens=%d maps=%d played=%u late=%u holes=%u\n", gAudio.volumeMilli, gAudio.want,
            gAudio.volumeSets, native_audio_service[1], native_audio_service[5],
            (unsigned)(gAudio.completed * kPeriodFrames), gAudio.late, gAudio.holes);
}

// Fills one free region with the tone and asks the DSP to play it. The write is not a waited command: q6asm_write sends it with
// NO_WAIT_CMD and the buffer only comes back through the write-done event, whose token names the buffer (q6asm.c:9684-9690, :2203).
void issueWrite() {
    uint8_t packet[APR_HEADER_BYTES + 40];
    uint32_t region = 0;
    for (;; ++region) {
        if (region >= kRegions) return;                       // every region is still held by the DSP
        if (!(gAudio.busyMask & (1u << region))) break;
    }
    int16_t *samples = reinterpret_cast<int16_t *>(gAudio.pcmVa + (uint64_t)region * kRegionBytes);
    // Three sources, in the order the path comes up: before the service is published the boot tone is the audible proof that the ladder
    // carries samples; after it the ring is the source while the audio server has output running, and a stopped stream plays silence
    // rather than a tone nobody asked for.
    const uint64_t start = gAudio.issued * kPeriodFrames;
    if (gAudio.serving && (gAudio.want & ShengAudioOutputRunning)) { takeRing(samples, start); watchRing(start, samples); }
    else if (gAudio.serving) memset(samples, 0, kRegionBytes);
    else {
        for (uint32_t i = 0; i < kPeriodFrames; ++i) {
            const int16_t value = kTone[gAudio.toneIndex & (kToneSteps - 1)];
            gAudio.toneIndex++;
            for (uint32_t c = 0; c < kChannels; ++c) samples[i * kChannels + c] = value;
        }
    }
    gAudio.issued++;
    cleanToPoc(gAudio.pcmVa + (uint64_t)region * kRegionBytes, kRegionBytes);
    const uint32_t token = apr_asm_token(kSession, kStream, (uint8_t)region, ASM_TOKEN_NO_WAIT);
    // seq_id is the buffer index, not a running count (q6asm.c: write.seq_id = port->dsp_buf), and the flags field the DSP sees is
    // 0x80000000 | the driver's flag, so a plain PCM write with no timestamp carries bit 31 set and nothing else (:9693-9699).
    const uint32_t n = apr_asm_write(packet, sizeof packet, token, kSession, kStream,
                                     (gAudio.pcmPa + (uint64_t)region * kRegionBytes) | kSidTag,
                                     gAudio.memHandle, kRegionBytes, region, 0, 0x80000000u);
    if (!n) {failed(StPlaying); gAudio.pace->cancelTimeout(); return;}
    if (sheng_adsp_send(gAudio.channel, packet, n)) bump(12);
    gAudio.busyMask |= 1u << region;
    gAudio.filled++;
    record();
}

void sendStep() {
    const Step &s = kLadder[gAudio.step];
    uint8_t packet[APR_MAX_PACKET];
    gAudio.token = apr_pending_add(&gAudio.pending, s.request, s.response);
    if (!gAudio.token) {failed(s.step); return;}
    const uint32_t n = s.encode(packet, sizeof packet, gAudio.token);
    if (!n) {failed(s.step); return;}
    if (sheng_adsp_send(gAudio.channel, packet, n)) bump(12);   // the answer timer still counts it
    gAudio.answer->setTimeoutMS(kAnswerMs);
    record();
}
void nextStep() {
    gAudio.answer->cancelTimeout();
    gAudio.tries = 0;
    reached(kLadder[gAudio.step].step);
    for (gAudio.step++; gAudio.step < (int)kSteps; ++gAudio.step)
        if (kLadder[gAudio.step].level <= (int)gAudio.level) break;
    if (gAudio.step >= (int)kSteps) {                       // the ladder is done: start the stream
        native_audio_stats[0] = StPlaying;
        // The vendor unmutes the amplifiers after the stream exists, so this is the point where their on-scene belongs; counted on
        // its own line so one flash answers the slot-geometry question and the amplifier-drive question separately.
        kprintf("[apr] pa=%u\n", dby_smartpa_play());
        // The audio server's half comes up with the stream: the rings have to exist before the plug-in can be handed them, and the
        // timeline's zero is the first region, which the pace timer issues on its first round below. A page that cannot be allocated or
        // a service that cannot be published leaves the tone in place -- the ladder is still proven, only the OS side is missing.
        if (gAudio.level >= 3 && !gAudio.serving) {
            if (!setupShared()) kprintf("[apr] os=page fail\n");
            else if (!publishService()) kprintf("[apr] os=publish fail\n");
            else {
                __atomic_or_fetch(&gAudio.ctl->state, ShengAudioOutputReady, __ATOMIC_ACQ_REL);
                __atomic_store_n(&gAudio.serving, 1u, __ATOMIC_RELEASE);
                kprintf("[apr] os=ready ticks=%u per frame\n", (unsigned)(gAudio.ctl->ticks_per_frame_q32 >> 32));
            }
        }
        gAudio.pace->setTimeoutMS(kRetryMs);
        return;
    }
    sendStep();
}
void outcome(int result, uint32_t request, uint32_t status) {
    if (result == AprPUnknown) return;
    if (gAudio.step < 0 || gAudio.step >= (int)kSteps) return;   // the ladder is done, nothing awaits an answer
    // An answer that names a step we already left is not an answer to the step we are on; ignore it rather than skip a rung.
    if (result != AprPResult && request != kLadder[gAudio.step].request) return;
    if (result == AprPAck || (result == AprPResult && status == APR_RSP_ACCEPTED)) {nextStep(); return;}
    // refused, an event where the awaited response was due, or a status we did not ask about: try again, then give up on this step.
    gAudio.answer->cancelTimeout();
    if (++gAudio.tries >= kTries) {failed(kLadder[gAudio.step].step); return;}
    gAudio.retry->setTimeoutMS(kRetryMs);
}

// ---- the DSP thread's callbacks: copy and signal, nothing else ----
void dspState(void *, int up) {
    __atomic_store_n(&gAudio.up, up ? 1u : 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gAudio.changed, 1u, __ATOMIC_RELEASE);
    gAudio.inbox->interruptOccurred(nullptr, nullptr, 0);
}
void dspReceive(void *, const uint8_t *data, uint32_t length) {
    IOSimpleLockLock(gAudio.ringLock);
    if (gAudio.ringCount < kRing) {
        auto &slot = gAudio.ring[(gAudio.ringHead + gAudio.ringCount) % kRing];
        slot.length = length < kMessage ? length : kMessage;
        memcpy(slot.data, data, slot.length);
        gAudio.ringCount++;
    } else {
        bump(11);
    }
    IOSimpleLockUnlock(gAudio.ringLock);
    gAudio.inbox->interruptOccurred(nullptr, nullptr, 0);
}

void inboxFired(OSObject *, IOInterruptEventSource *, int) {
    if (__atomic_exchange_n(&gAudio.changed, 0u, __ATOMIC_ACQUIRE)) {
        const bool up = __atomic_load_n(&gAudio.up, __ATOMIC_ACQUIRE) != 0;
        if (up && !gAudio.wasUp) {
            bump(8);
            gAudio.upAt = mach_absolute_time();
            apr_pending_init(&gAudio.pending);
            gAudio.step = 0;
            if (gAudio.level >= 2 && !muxPads()) {failed(StPads); return;}
            if (gAudio.level >= 2) reached(StPads);
            native_audio_stats[0] = StUp;
            sendStep();
        } else if (!up && gAudio.wasUp) {
            bump(9);
            gAudio.answer->cancelTimeout();
            gAudio.retry->cancelTimeout();
            gAudio.pace->cancelTimeout();
            native_audio_stats[0] = StChannel;
        }
        gAudio.wasUp = up;
    }
    for (;;) {
        uint8_t data[kMessage];
        uint32_t length = 0;
        IOSimpleLockLock(gAudio.ringLock);
        if (gAudio.ringCount) {
            auto &slot = gAudio.ring[gAudio.ringHead];
            length = slot.length;
            memcpy(data, slot.data, length);
            gAudio.ringHead = (gAudio.ringHead + 1) % kRing;
            gAudio.ringCount--;
        }
        IOSimpleLockUnlock(gAudio.ringLock);
        if (!length) break;
        AprMessage m;
        if (apr_decode(data, length, &m)) {bump(11); continue;}
        uint32_t request = 0, status = 0;
        if (m.opcode == APR_OP_AVCSS_GET_FWK_VERSION_RSP) {           // what wire family each service on this ADSP speaks
            uint32_t services = 0;
            apr_avcs_service_api(&m, APRV2_IDS_SERVICE_ID_ADSP_AFE_V, &gAudio.afeApi, &services);
            apr_avcs_service_api(&m, APRV2_IDS_SERVICE_ID_ADSP_ADM_V, &gAudio.admApi, &services);
            apr_avcs_service_api(&m, APRV2_IDS_SERVICE_ID_ADSP_ASM_V, &gAudio.asmApi, &services);
        }
        if (m.opcode == APR_OP_ADM_DEVICE_OPEN_RSP_V8) {
            uint32_t openStatus = 0;
            if (!apr_adm_device_open_rsp(&m, &openStatus, &gAudio.coppId))
                kprintf("[apr] copp=%d status=%d\n", gAudio.coppId, openStatus);
        }
        if (m.opcode == APR_OP_ASM_MEM_MAP_RSP) {
            uint32_t handle = 0;
            if (!apr_asm_mem_map_rsp(&m, &handle)) gAudio.memHandle = handle;
        }
        uint8_t doneRegion = 0;
        uint32_t doneStatus = 0;
        if (!apr_asm_write_done(&m, &doneRegion, &doneStatus)) {      // the write was never a waited request: free its region
            if (doneRegion < kRegions && (gAudio.busyMask & (1u << doneRegion))) {
                gAudio.busyMask &= ~(1u << doneRegion);
                gAudio.completed++;
            }
            record();
            continue;
        }
        outcome(apr_pending_reply(&gAudio.pending, &m, &request, &status), request, status);
    }
}
void answerFired(OSObject *, IOTimerEventSource *) {
    if (gAudio.step < 0 || gAudio.step >= (int)kSteps) return;
    apr_pending_timeout(&gAudio.pending, gAudio.token);
    record();
    if (++gAudio.tries >= kTries) {failed(kLadder[gAudio.step].step); return;}
    gAudio.retry->setTimeoutMS(kRetryMs);
}
void retryFired(OSObject *, IOTimerEventSource *) {
    if (gAudio.wasUp && gAudio.step >= 0 && gAudio.step < (int)kSteps) sendStep();
}
void paceFired(OSObject *, IOTimerEventSource *) {
    if (!gAudio.wasUp || !gAudio.pcmVa) return;
    for (unsigned n = 0; n < kRegions; ++n) issueWrite();   // a region the DSP still holds is skipped inside
    serviceRow();                                           // the audio server's side, once per change
    gAudio.pace->setTimeoutMS(kRetryMs);   // 100 ms, a little over one 85 ms round of four regions
}

// The PCM ring the DSP reads: physically contiguous, and the audio stream ids of this board's ION heap
// (kona-audio.dtsi:5-6, iommus = <&apps_smmu 0x1801 0x0> with qcom,smmu-sid-mask = 0xf) left in pass-through so the address we hand the
// DSP is the physical one. dby-audio-pa uses a fixed low address instead, for the case the ADSP refuses ours.
bool setupPcm() {
    uint64_t chosen = 0;
    if (PE_parse_boot_argn("dby-audio-pa", &chosen, sizeof chosen) && chosen) {
        IOMemoryMap *map = nullptr;
        volatile uint8_t *va = nullptr;
        if (!mapRegion(chosen, kRegions * kRegionBytes, map, va, kIOMapWriteCombineCache)) return false;
        gAudio.pcmVa = const_cast<uint8_t *>(va);      // written with memcpy, then cleaned with dc cvac
        gAudio.pcmPa = chosen;
        return true;
    }
    gAudio.pcm = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                  (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous,
                  kRegions * kRegionBytes, 0xfffffffffffff000ull);
    if (!gAudio.pcm || gAudio.pcm->prepare() != kIOReturnSuccess) return false;
    gAudio.pcmVa = static_cast<uint8_t *>(gAudio.pcm->getBytesNoCopy());
    gAudio.pcmPa = gAudio.pcm->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    return gAudio.pcmVa && gAudio.pcmPa;
}
struct SmmuWindow {volatile uint8_t *va;};
IOMemoryMap *gSmmuMap;                                       // its own mapping: the pad controller's map lives in gTlmmMap
uint32_t smmuRd(void *ctx, unsigned at) {return *reinterpret_cast<volatile uint32_t *>(static_cast<SmmuWindow *>(ctx)->va + at);}
void smmuWr(void *ctx, unsigned at, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(static_cast<SmmuWindow *>(ctx)->va + at) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
bool setupSmmu() {
    volatile uint8_t *va = nullptr;
    SmmuWindow window;
    if (!mapRegion(TB_APPS_SMMU_BASE, TB_APPS_SMMU_SIZE, gSmmuMap, va)) return false;
    window.va = va;
    const struct smmu_io io = {&window, smmuRd, smmuWr};
    // Exact-match bypass on the heap's own stream id: the second iommus cell is 0, and the whole ring is the range it must reach.
    struct smmu_plan plan = smmu_plan_bypass(&io, kLpassSid, gAudio.pcmPa, kRegions * kRegionBytes);
    const char *error = plan.error ? plan.error : smmu_configure_bypass(&io, &plan, kLpassSid);
    // The numbers, not just the name: whether the group took the value is the one thing this row cannot be read without, and a
    // slot of 0xffffffff is what "no free stream-match group" leaves behind, so it is reported rather than indexed.
    if (plan.slot > 127) kprintf("[apr] smmu %s slot=none bank=%u\n", error ? error : "ok", plan.bank);
    else {
        const uint32_t smr = io.rd(io.ctx, 0x800 + 4 * plan.slot), s2cr = io.rd(io.ctx, 0xc00 + 4 * plan.slot);
        kprintf("[apr] smmu %s slot=%u bank=%u smr=0x%08x s2cr=0x%08x\n", error ? error : "ok", plan.slot, plan.bank, smr, s2cr);
    }
    return error == nullptr;
}
}  // namespace

// The two entries package/ShengAudioService.cpp calls: the shared objects, for the audio server's task to map, and its start and stop
// requests. The descriptors are retained here and released never: the plug-in's task keeps its map of a ring after the service is
// closed, and the timeline's anchors have to stay meaningful for the whole boot.
extern "C" IOBufferMemoryDescriptor *sheng_audio_memory(uint32_t type) {
    if (!__atomic_load_n(&gAudio.serving, __ATOMIC_ACQUIRE)) return nullptr;
    IOBufferMemoryDescriptor *m = type == ShengAudioMemoryControl ? gAudio.control : type == ShengAudioMemoryOutput ? gAudio.outRing :
                                  type == ShengAudioMemoryInput ? gAudio.inRing : nullptr;
    if (m) m->retain();
    return m;
}

// [apr] requests: the plug-in's last 16 requests, each millisecond since boot << 4 | the selector, oldest first from [16] (the next slot).
extern "C" {volatile uint64_t native_audio_requests[17];}
extern "C" int sheng_audio_request(uint32_t selector) {
    if (!__atomic_load_n(&gAudio.serving, __ATOMIC_ACQUIRE)) return -1;
    const uint64_t slot = __atomic_fetch_add(&native_audio_requests[16], 1, __ATOMIC_RELAXED) % 16;
    native_audio_requests[slot] = nowMs() << 4 | (selector & 15);
    switch (selector) {
    case ShengAudioStartOutput: __atomic_or_fetch(&gAudio.want, ShengAudioOutputRunning, __ATOMIC_ACQ_REL); break;
    case ShengAudioStopOutput: __atomic_and_fetch(&gAudio.want, ~ShengAudioOutputRunning, __ATOMIC_ACQ_REL); break;
    case ShengAudioStartInput: __atomic_or_fetch(&gAudio.want, ShengAudioInputRunning, __ATOMIC_ACQ_REL); break;
    case ShengAudioStopInput: __atomic_and_fetch(&gAudio.want, ~ShengAudioInputRunning, __ATOMIC_ACQ_REL); break;
    default: return -1;
    }
    // What the state bit says is what the pump feeds the port. The ASM stream itself runs from the ladder to the end of the boot, so
    // a stop request is not a stopped DSP: it is the port playing the ring's cleared frames. Input is never claimed ready, because this
    // board's ladder opens a playback ADM device only and the capture ring stays empty.
    if (gAudio.ctl) {
        uint32_t state = ShengAudioOutputReady;
        if (gAudio.want & ShengAudioOutputRunning) state |= ShengAudioOutputRunning;
        __atomic_store_n(&gAudio.ctl->state, state, __ATOMIC_RELEASE);
    }
    return 0;
}

extern "C" void sheng_audio_probe(void) {
    unsigned mode = 0;
    if (!sheng_adsp_channel || !sheng_adsp_send) return;
    // Named dby-audio, not the sheng-audio gate the SM8550 AudioProbe.cpp reads: that file is another machine's silicon and is
    // deliberately not compiled for this board (build-gop-legacy-display.py:235-247), and waking it with our argument would have it
    // vote an SM8550 GCC register set. board.toml:216-219 is the same rule in words.
    if (!PE_parse_boot_argn("dby-audio", &mode, sizeof mode) || (mode != 1 && mode != 2) || native_audio_stats[0]) return;
    unsigned level = 3;
    PE_parse_boot_argn("dby-apr", &level, sizeof level);
    if (level < 1 || level > 3) level = 3;
    native_audio_stats[13] = (int32_t)level;
    native_audio_stats[0] = StStarted;
    unsigned pa = 0;
    if (level >= 3 && PE_parse_boot_argn("dby-apr-off", &pa, sizeof pa) && pa) level = 2;   // the ladder without a stream, for one run
    unsigned nosmmu = 0;
    PE_parse_boot_argn("dby-apr-nosmmu", &nosmmu, sizeof nosmmu);
    // Everything the callbacks touch goes up first: the pad mux runs from inboxFired and a write is issued from paceFired, so both
    // need the event sources and their maps to already exist.
    gAudio.level = level;
    gAudio.volumeQ13 = ASM_Q13_UNITY;   // 0 dB, the value the vendor's volume table tops out at; the tone is already at 8000/32767
    gAudio.loop = IOWorkLoop::workLoop();
    gAudio.ringLock = IOSimpleLockAlloc();
    if (!gAudio.loop || !gAudio.ringLock) {native_audio_stats[0] = -StStarted; return;}
    gAudio.answer = IOTimerEventSource::timerEventSource(gAudio.loop, answerFired);
    gAudio.retry = IOTimerEventSource::timerEventSource(gAudio.loop, retryFired);
    gAudio.pace = IOTimerEventSource::timerEventSource(gAudio.loop, paceFired);
    gAudio.inbox = IOInterruptEventSource::interruptEventSource(gAudio.loop, inboxFired);
    if (!gAudio.answer || !gAudio.retry || !gAudio.pace || !gAudio.inbox || gAudio.loop->addEventSource(gAudio.answer) ||
        gAudio.loop->addEventSource(gAudio.retry) || gAudio.loop->addEventSource(gAudio.pace) ||
        gAudio.loop->addEventSource(gAudio.inbox)) {native_audio_stats[0] = -StStarted; return;}
    if (level >= 2) {
        if (!mapRegion(TB_TLMM_BASE, 0x1000000, gTlmmMap, gTlmm)) {native_audio_stats[0] = -StPads; return;}
    }
    // dby-apr-off and dby-apr-nosmmu are separate switches and get separate variables: sharing one made the first also skip the
    // stream mapping, which is a different failure and reads as a DSP refusal.
    if (level >= 3 && !setupPcm()) {native_audio_stats[0] = -StMapped; return;}
    // A refused bypass bank is reported by setupSmmu's own row rather than fatal: the ladder is what says whether the DSP can reach
    // the ring with the mapping the boot left behind, and stopping here spends the whole boot to learn nothing new.
    if (level >= 3 && !nosmmu) (void)setupSmmu();
    static const ShengAdspClient client = {dspState, dspReceive};
    gAudio.channel = sheng_adsp_channel("apr_audio_svc", kIntentSize, kIntentCount, mode == 2, &client, nullptr);
    if (!gAudio.channel) {native_audio_stats[0] = -StChannel; return;}
    native_audio_stats[0] = StChannel;
}
