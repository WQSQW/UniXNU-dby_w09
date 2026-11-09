// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Kernel side of DbyTorch (DbyTorch.h): the windows, the thread and the arguments, around the
// portable core in DbyTorchCore.c. The shape is PaganiTorch's PaganiTorch.cpp; every number the core
// uses is in DbyTorchRegs.h. dby_torch_start reads the boot arguments and returns at once without
// them: with dby-torch absent or 0 nothing is mapped, allocated, written or registered. Otherwise it
// allocates the core, starts one kernel thread, "dby-torch", and returns. That thread maps the v5
// arbiter's core, configuration and this EE's observer windows (all three are read-only needs except
// the observer's command byte), runs the core's discovery and identity, and - once the core has found
// the flash channel's APID and proved the write EE - maps exactly one RW channel page
// (chnls + DBT_ARB_PAGE * apid, DbyTorchSpmi.c through ioChanMap). Then it serves level requests
// and, while the LED is on, polls the fault registers and the 5-minute deadline. All register access
// happens on that thread, so nothing ever touches SPMI from two CPUs at once and no caller waits on
// the bus. A dry run, a refusal or a fault unmaps everything and ends the thread; every request then
// returns -19. Every step is a "[torch]" line on kprintf - this board's COM4 mirrors kprintf rows and
// no IOLog row (DbyBattery.cpp:77-86) - and a word in dby_torch_status.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "DbyTorch.h"
#include "DbyTorchCore.h"
#include "DbyTorchIos.h"
#include "DbyTorchText.h"
}

extern "C" {
volatile int32_t dby_torch_status[16];
}

namespace {
enum : uint32_t {
    kIdleMs = 60000,                  // nothing to poll while off: requests wake the thread
    kAnswerMs = 200,                  // a caller waits this long for the thread
    kObsSize = DBT_ARB_OBS_EE_STRIDE, // this EE's observer channels
    kPageSize = DBT_ARB_PAGE,
};

/* One mapped window, with the size it was mapped at: every offset the core asks for is checked
 * against it, so an offset that leaves the window reads as an error status and is never written. */
struct Window {
    IOMemoryMap *map;
    volatile uint8_t *va;
    uint32_t size;
};
Window gCoreWin, gCnfgWin, gObsWin, gChanWin;
DbtCore *gTorch;
DbtConfig gConfig;
DbtSpmiIo gIo;
DbtOps gOps;
IOLock *gCaller;
uint32_t gTestMs;
volatile uint32_t gStarted, gReady, gPending;
// The one request in flight: gCaller's holder writes it, the thread answers with its sequence number.
volatile uint32_t gLevel, gRequest, gAnswered;
const char *volatile gWho;
volatile int32_t gAnswer;

void trace(const char *line) {kprintf("%s\n", line);}

unsigned argument(const char *name, unsigned fallback)
{
    unsigned value = fallback;
    return PE_parse_boot_argn(name, &value, sizeof value) ? value : fallback;
}

uint64_t nowUs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000u;
}

bool mapOne(Window &w, uint64_t base, uint32_t size)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    w.size = w.va ? size : 0;
    return w.va != nullptr;
}
void unmapOne(Window &w)
{
    w.va = nullptr;
    w.size = 0;
    OSSafeReleaseNULL(w.map);
}
void unmapAll()
{
    unmapOne(gChanWin);
    unmapOne(gObsWin);
    unmapOne(gCnfgWin);
    unmapOne(gCoreWin);
}

uint32_t rd32(const Window &w, uint32_t at)
{
    if (!w.va || at + 4u > w.size) return 0xeu;
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(w.va + at);
    __asm__ volatile("dmb sy" ::: "memory");
    return value;
}
void wr32(const Window &w, uint32_t at, uint32_t value)
{
    if (!w.va || at + 4u > w.size) return;
    *reinterpret_cast<volatile uint32_t *>(w.va + at) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

// ---- the core's SPMI windows (DbyTorchSpmi.h) ----
uint32_t ioCore(void *, uint32_t at) {return rd32(gCoreWin, at);}
uint32_t ioCnfg(void *, uint32_t at) {return rd32(gCnfgWin, at);}
uint32_t ioObs(void *, uint32_t at) {return rd32(gObsWin, at);}
void ioObsPut(void *, uint32_t at, uint32_t value) {wr32(gObsWin, at, value);}
int ioChanMap(void *, unsigned apid)
{
    if (apid >= DBT_ARB_MAX_APIDS || gChanWin.va) return -22;
    const uint64_t base = DBT_ARB_CHNLS + uint64_t(kPageSize) * apid;
    return mapOne(gChanWin, base, DBT_ARB_CHNL_SZ) ? 0 : -12;
}
uint32_t ioChan(void *, uint32_t at) {return rd32(gChanWin, at);}
void ioChanPut(void *, uint32_t at, uint32_t value) {wr32(gChanWin, at, value);}
void ioDelay(void *, unsigned us) {IODelay(us);}

// ---- the core's clock and log ----
uint64_t opNow(void *) {return nowUs();}
void opTrace(void *, const char *line) {trace(line);}

// ---- the thread ----
void wake()
{
    __atomic_fetch_or(&gPending, 1u, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gPending);
}
void waitMs(uint32_t ms)
{
    if (__atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL)) return;
    assert_wait_timeout((event_t)&gPending, THREAD_UNINT, ms ? ms : 1, kMillisecondScale);
    if (__atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL)) {
        clear_wait(current_thread(), THREAD_AWAKENED);
        return;
    }
    thread_block(THREAD_CONTINUE_NULL);
    __atomic_exchange_n(&gPending, 0u, __ATOMIC_ACQ_REL);
}

void exportStatus()
{
    int32_t words[16];
    dbt_core_export(gTorch, words);
    for (unsigned i = 0; i < 16; ++i) dby_torch_status[i] = words[i];
}

void serveRequest(uint32_t &served)
{
    const uint32_t seq = __atomic_load_n(&gRequest, __ATOMIC_ACQUIRE);
    if (seq == served) return;
    served = seq;
    const int rc = dbt_core_set(gTorch, __atomic_load_n(&gLevel, __ATOMIC_RELAXED), 0, gWho);
    __atomic_store_n(&gAnswer, rc, __ATOMIC_RELAXED);
    __atomic_store_n(&gAnswered, seq, __ATOMIC_RELEASE);
}

void torchThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "dby-torch");
    if (!mapOne(gCoreWin, DBT_ARB_CORE, DBT_ARB_CORE_SZ) ||
        !mapOne(gCnfgWin, DBT_ARB_CNFG, DBT_ARB_CNFG_SZ) ||
        !mapOne(gObsWin, DBT_ARB_OBS + uint64_t(kObsSize) * DBT_EE, kObsSize)) {
        unmapAll();
        dby_torch_status[1] = -5;
        trace("[torch] FAIL spmi windows not mapped: torch off");
        return;
    }
    const int rc = dbt_core_init(gTorch, &gConfig, &gOps, &gIo);
    exportStatus();
    if (rc) {   // 1: the dry run's log is out; negative: refused or failed (the core logged where)
        unmapAll();
        trace(rc == 1 ? "[torch] dry run done: nothing written, torch off" : "[torch] off");
        return;
    }
    __atomic_store_n(&gReady, 1u, __ATOMIC_RELEASE);
    if (gTestMs) dbt_core_set(gTorch, DBT_LEVEL_MAX, gTestMs, "test");
    uint32_t served = 0;
    uint32_t sleep = dbt_core_poll(gTorch);
    exportStatus();
    for (;;) {
        if (gTorch->state == DBT_ST_FAULT) {   // stopped by the core; off until reboot
            __atomic_store_n(&gReady, 0u, __ATOMIC_RELEASE);
            serveRequest(served);              // a caller still waiting gets the fault, not the timeout
            exportStatus();
            unmapAll();
            trace("[torch] off after a fault");
            return;
        }
        waitMs(sleep ? sleep : kIdleMs);
        serveRequest(served);
        sleep = dbt_core_poll(gTorch);
        exportStatus();
    }
}
}  // namespace

extern "C" void dby_torch_start(void)
{
    const unsigned mode = argument("dby-torch", 0);
    // The iOS switch first (DbyTorchIos.h, DbyTorchIos.cpp): it only plans here, the service and the node come just before launchd.
    dby_torch_ios_plan(argument("dby-ti", 0), mode);
    if (!dbt_config_make(&gConfig, mode, DBT_EE, DBT_TORCH_MAX_MA, argument("dby-torch-max-ma", 0),
                         argument("dby-torch-log", 1)))
        return;
    if (__atomic_exchange_n(&gStarted, 1u, __ATOMIC_ACQ_REL)) return;
    dby_torch_status[1] = 1;
    gTestMs = mode == DBT_MODE_ON ? dbt_test_ms(argument("dby-torch-test", 0)) : 0;
    gCaller = IOLockAlloc();
    gTorch = static_cast<DbtCore *>(IOMalloc(sizeof(DbtCore)));
    if (!gCaller || !gTorch) {
        dby_torch_status[1] = -7;
        trace("[torch] FAIL no memory: torch off");
        return;
    }
    gIo = {nullptr, ioCore, ioCnfg, ioObs, ioObsPut, ioChanMap, ioChan, ioChanPut, ioDelay};
    gOps = {nullptr, opNow, opTrace};
    char line[160];
    DbtText t;
    dbt_text_start(&t, line, sizeof line);
    dbt_text_str(&t, "[torch] start");
    dbt_text_kd(&t, "mode", (int64_t)mode);
    dbt_text_kd(&t, "max_ma", (int64_t)gConfig.max_ma);
    dbt_text_kd(&t, "hard_ma", (int64_t)gConfig.hard_ma);
    dbt_text_kx(&t, "ppid", gConfig.ppid);
    dbt_text_kx(&t, "leds", gConfig.led_mask);
    dbt_text_kd(&t, "log", (int64_t)gConfig.log);
    dbt_text_kd(&t, "test_ms", (int64_t)gTestMs);
    trace(line);
    thread_t thread = nullptr;
    if (kernel_thread_start(torchThread, nullptr, &thread) != KERN_SUCCESS) {
        dby_torch_status[1] = -6;
        trace("[torch] FAIL thread: torch off");
        return;
    }
    thread_deallocate(thread);
}

extern "C" int dby_torch_request(uint32_t level, const char *who)
{
    if (level > DBT_LEVEL_MAX) return -22;
    if (!__atomic_load_n(&gReady, __ATOMIC_ACQUIRE)) return -19;
    IOLockLock(gCaller);
    __atomic_store_n(&gLevel, level, __ATOMIC_RELAXED);
    gWho = who;
    const uint32_t seq = __atomic_load_n(&gRequest, __ATOMIC_RELAXED) + 1;
    __atomic_store_n(&gRequest, seq, __ATOMIC_RELEASE);
    wake();
    int rc = -110;
    for (uint32_t ms = 0; ms <= kAnswerMs; ++ms) {
        if (__atomic_load_n(&gAnswered, __ATOMIC_ACQUIRE) == seq) {
            rc = __atomic_load_n(&gAnswer, __ATOMIC_RELAXED);
            break;
        }
        if (!__atomic_load_n(&gReady, __ATOMIC_ACQUIRE) && ms) {   // the thread ended before it saw this
            rc = -19;
            break;
        }
        IOSleep(1);
    }
    IOLockUnlock(gCaller);
    return rc;
}

extern "C" int dby_torch_set(uint32_t level) {return dby_torch_request(level, "api");}

extern "C" int dby_torch_level(void)
{
    return __atomic_load_n(&gReady, __ATOMIC_ACQUIRE) ? dby_torch_status[2] : -19;
}
