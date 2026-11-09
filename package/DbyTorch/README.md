# DbyTorch

The DBY-W09 (Huawei MatePad 11, SM8250/kona) LED torch from XNU: PM8150L's flash LED module (SPMI
USID 5, block base `0xd300`, the 4-channel flash-v2 block; this unit's torch is on channel 0 only)
driven over the SM8250 v5 SPMI PMIC arbiter as apps EE 0, with a kernel API.

Status, measured 2026-11-09 rather than carried forward: the portable core and the glue pass
`scripts/check-dby-torch-host.py`, archive gates included, against a real tree's XNU compile records
(see `## Building and testing`). The driver is wired into this device repository and is in that tree's
kernel — `board.toml` gives `scripts/build-dby-torch.py` an `archives` slot,
`configs/dby_w09_defconfig:130` names `build/dby-torch/libDbyTorch.a`, `package/BootProgress.c:1134`
calls `dby_torch_start()`, and `build/xnu/ios-obj/mach.development.bcm2837` defines the 20 entry
points. Two things are not: that kernel is the one built before the `DbyTorchIos.*` sources existed, so
its archive has the core and the driver but not the iOS glue (the next official kernel build links all
five objects, which is the archive the check builds today), and no `[torch]` row has been read on the
device — `dby-torch=2` is armed, the payload carrying it booted normally, and this board's USB debug
console (VID_1209/PID_316d) never enumerated, so not one register read has been seen and mode 1 is not
yet earned. Default off: without the boot arguments nothing runs, maps, allocates or writes. The
iOS-facing half — the Control Center flashlight button reaching this driver — has its kernel side
written (`## The flashlight of iOS`): `DbyTorchIos.*` is that side, the MediaCapture mount and the
`H6ISP.mediacapture` plug-in are the other two, and none of it is armed until the boot arguments carry
`dby-ti=1`.

## Sources (facts only, no code copied)

| Tag | Source | Used for |
| --- | --- | --- |
| [V] | this board's vendor driver for the module, `drivers/leds/flash/leds-qpnp-flash-v2.c` (GPL-2.0-only) | the register map, the current formula and its limits, the PM8150L resolution erratum, the on and off sequences, the fault bits, the thermal derate |
| [A] | Linux `drivers/spmi/spmi-pmic-arb.c` | the v5 arbiter: APID and ownership tables, observer and request channels, command and status words, `pmic_arb_wait_for_done`'s difference between a timeout and an error bit |
| [D] | this unit's stock DTBO (`fragment@4/__overlay__/qcom,pm8150l@5/qcom,leds@d300/…`) plus `arch/arm64/boot/dts/vendor/qcom/kona.dtsi` | `reg = <0xd300 0x100>`, `qcom,led-switch_0 qcom,led-mask = <0x1>`, `qcom,current-ma = <0x12c>`, `qcom,max-current = <0x1f4>`, `qcom,ires-ua = <0x30d4>`, `qcom,thermal-derate-current = <0xc8 0x1f4 0x3e8>`, the arbiter's four windows and `qcom,ee = <0>` |
| ours | `package/DbyBattery.cpp` (running on this tablet) | the arbiter windows and the two command-word ladders as this board actually answers them, and that COM4 mirrors `kprintf` rows only |

Registers and every board number are in `DbyTorchRegs.h`, each with the line or cell it came from.
The compatible string of this node is `qcom,qpnp-flash-led-v2`, so [V] and not `leds-qcom-flash.c`
is this hardware's manual.

## Safety

- **Writes are whitelisted in `DbyTorchSpmi.c`, not by the caller**: only PM8150L's flash block (the
  one APID whose PPID is `0x5d3`), only the registers the torch sequence uses — `IRES`,
  `STROBE_CTRL`, `TGR_CURRENT`, `SAFETY_TMR`, `MOD_CTRL`, `EN_LED_CTRL` — single bytes, within this
  board's `led-mask`, and only these values: `TGR_CURRENT` at most the code of the module's own
  `qcom,max-current`, `SAFETY_TMR` the vendor's disabled code, `STROBE_CTRL` the software strobe,
  `MOD_CTRL` 0x00 or 0x80, `EN_LED_CTRL` within the mask.
- **An interlock**: the channel is switched on only after this gate itself wrote the resolution, the
  strobe, the target current and the timer since the channel was mapped, so a current left behind by
  Android can never be switched on. Those bytes are overwritten first and every one is read back
  before the light goes on.
- **Before any write**: the arbiter answers as v5, PPID `0x5d3` maps to an APID whose write EE is
  apps EE 0, the observer's `INT_RT_STS` and both status bytes say the block is idle as far as this
  board's channel is concerned. Before **every** write the APID table entry and the owner are read
  again; an APID that moved, or a channel owned by another EE, stops the run.
- **Current**: one channel, 300 mA at level 1000 by default ([D] `qcom,current-ma`), capped at
  [D] `qcom,max-current` = 500 mA, and the resolution law including `PM8150L_IRES_WA` ([V]:1367-1370)
  is applied before the code is derived, so a request never rounds up. A level of 1 is one resolution
  step (12.5 mA), not 0.3 mA.
- **On-time**: an "on" ends by itself after 5 minutes. Changing the level does not extend it; off and
  on again does.
- **Faults**: `STATUS1` (short/open) and `STATUS2`'s VPH-droop bit are [V]'s two, and [V] reads them
  under one condition - its led-fault-irq branch (`leds-qpnp-flash-v2.c:1992-2013`), the interrupt the
  board DTs put at `INT_RT_STS` bit 0. So the port stops the LED only while that line says so, and then
  with the ordinary switch_disable ladder, leaving the driver ready for the next request; `INT_RT_STS`
  alone and the thermal OTST level are evidence and a derate, never a stop. A bus failure, timeout,
  refusal or failed read-back is the other case: it switches the LED off (`EN_LED_CTRL` and `MOD_CTRL`,
  the one write allowed after the bus closed) and keeps the driver off until reboot.
- **Heat derates, it does not stop**: [D] sets `qcom,thermal-derate-en` with 200/500/1000 mA, and
  [V]:1290-1318 turns a non-zero OTST level into a reprogram of `TGR_CURRENT` alone. One deviation
  from [V] is recorded in `DbyTorchRegs.h`: the vendor's `thermal_derate_current[otst >> 1]` reads
  one cell past the three at OTST 6 and 7, and this port clamps to the last cell instead.
- **The boot never waits**: `dby_torch_start` returns at once; every register access is on the
  driver's own thread, and every wait is bounded (a caller gets at most 200 ms, the arbiter poll is
  [A]'s 1000 iterations).
- No PMIC interrupt is enabled; the status bytes are read, not subscribed to.

## Sequences (`DbyTorchCore.c`)

- **Take-over** (from Android, which leaves the module enabled): `EN_LED_CTRL` and `MOD_CTRL` off
  through read-modify-write, both read back. 22 observer reads and 2 channel writes.
- **On** ([V] `qpnp_flash_led_switch_set`'s order): the channel's enable off, the module off,
  `IRES`, the channel's `STROBE_CTRL`, the channel's `TGR_CURRENT`, the channel's `SAFETY_TMR`, all
  four read back, `MOD_CTRL` 0x80, `EN_LED_CTRL`, the three status bytes read. 17 reads and 6 writes.
- **Off** ([V] `qpnp_flash_led_switch_disable`): the enable byte and the module byte read, cleared
  through the mask, `TGR_CURRENT` 0, `SAFETY_TMR` the disabled code, the enable, the module and the
  current read back. 9 reads and 4 writes.
- **Level change**: the on sequence again when the code or the resolution moves, one `TGR_CURRENT`
  pair when only the derate does; a full run — take-over, on, off — is 48 observer reads and 12
  channel writes, which is what `Test/core.c` fails one at a time.
- **Dry run** (`2`): discovery, identity, owner and a 16-byte register dump in the boot log through
  observer reads, plus one read of the module's own RW `STATUS`; 20 reads and no write.

## Boot arguments

| Argument | Meaning |
| --- | --- |
| `dby-torch=1` | on; `2`: dry run (discovery, identity, owner and a register dump in the log through observer reads, plus one read of the module's RW channel STATUS; nothing written); absent or anything else: off |
| `dby-torch-max-ma` | the current at level 1000; default 300 ([D] `qcom,current-ma`), at most 500 ([D] `qcom,max-current`), never below one resolution step |
| `dby-torch-test` | bring-up: on at level 1000 for this many ms (at most 1000) right after the initialization, then off (mode 1) |
| `dby-torch-log=0/1/2` | failures only / rationed (default: the first 8 changes, then one per 5 minutes, at most 24 a boot) / every change (at most 64) |
| `dby-ti=1` | the iOS flashlight surface (`## The flashlight of iOS`): the `AppleH6CamIn` service, its mediaserverd-only user client and `IODeviceTree:/product/camera` `flash`, made just before launchd. Absent or 0: `DbyTorchIos.cpp` never runs and logs nothing. 1 with `dby-torch` not 1 is refused in the log, and 1 without the plug-in visible on the root is refused too, so a button is never a button that does nothing |

Byte cost on the joined command line, measured 2026-11-09 against the device repository's own
`kernel_args` list (the payload joins its entries with one space, so an entry costs its own length
plus one separator, and the budget is 1000 bytes for the whole line): `dby-torch=2` and `dby-torch=1`
are 11 characters each and `dby-torch-test=100` is 18, while the mother's `pagani-torch-ios=1` would
cost 19 here and this board's long spelling 16. The line before this round was 49 entries and 989
bytes, 11 free, so the iOS switch under its long name needed 5 bytes taken from other arguments.
All 49 were then read at their readers (feeds/xnu, upstream/core, this repository's package+scripts,
the SoC feed and uefi-sheng) and none of them is dead: every `offline-legacy-*` row is a gate in
NativeLegacyDisplay.cpp (gop at :383 returns false without it, pm and phase at :98-102 stop the
display, register at :128, validate at :313, transfer at :149), `kt_fixed_zones=120` overrides XNU's
own 260 (zalloc_internal.h:485; the 120 branch is watchOS-or-KASAN only), `dk=0` overrides IOKit's
default-on DriverKit debug (IOKitDebug.h:134 kIODKEnable=1, IOUserServer.cpp:88),
`panic_on_crash_disabled=1` is what keeps a crashed daemon from panicking the board
(kern_exit.c:1695) and `-enable_kprintf_spam` is the only route that lets kprintf reach the console
at all (pe_kprintf.c:50). So the switch takes the short board name - the same rule that paid for
`dby-bl=1` where `sheng-backlight=1` would have cost twice - and the armed line measures 50 entries
and 998 bytes with 2 free, read back from core's own `payload.check_args` rather than counted by hand.

The first run on the device was `dby-torch=2` (a read-only take-over: it proved the windows, the
arbiter version, the APID, the write EE and the identity bytes and wrote nothing). The boot-time
self-test `dby-torch=1 dby-torch-test=100` costs 31 joined bytes and cannot be afforded, which is
why the visible light now goes through the API instead: `dby-torch=1 dby-ti=1`.

## Interfaces

- `dby_torch_set(level)` and `dby_torch_request(level, "who")` (`DbyTorch.h`): 0 off, 1..1000
  permille of the cap; any thread that may sleep; waits at most 200 ms for the driver's thread and
  returns what happened (`0`, `-19`, `-22`, `-5`, `-1`, `-110`).
- `dby_torch_level()`: the level now, or `-19` when the driver is not running.
- `dby_torch_status[16]`: the core's export (`DbyTorchCore.c`), documented in `DbyTorch.h`. Every
  step of the run is also a `[torch]` line on `kprintf`, which is what this board's COM4 mirrors.

## The flashlight of iOS

The chain, read on the root this board actually boots (iPadOS 17.7.11 / 21H461, `board.toml`'s
target) and measured on 2026-11-09 rather than copied from the mother:

- Control Center's FlashlightModule and the lock screen ask `+[AVFlashlight hasFlashlight]`, which is
  `MGGetBoolAnswer("camera-flash")`, which is the property `flash` existing on
  `IODeviceTree:/product/camera` — any value, its existence is the answer.
- AVFlashlight talks XPC to `FigFlashlight` in mediaserverd, which takes a device and a stream from
  `BWFigCaptureDeviceVendor` and sets that stream's `TorchLevel`.
- The vendor picks its capture class with `FigCaptureGetCaptureDeviceCreateFunction`, probing
  `AppleH16CamIn`, `AppleH13CamIn`, `AppleH10CamIn`, `AppleH9CamIn`, `AppleH6CamIn` in that order: the
  first class with an IOService of it in the registry makes it `dlopen`
  `/System/Library/MediaCapture/<class-short>ISP.mediacapture`. That order and that path are the
  mother's measurement (`refer/Pagani/patches/xnu/phone-torch-overlay.patch`, which says of her root
  that the directory is empty and the file is not in the shared cache); the table itself lives in
  CMCapture, and no shared cache of this build is in this repository, so on this root it is an
  answer the counters have to give, not something read here.
- Measured in this board's own pinned kernelcache (`out/ipsw/21H461/kernelcache.decompressed`,
  46,415,872 bytes, the file the 21H461 IPSW decompresses to): its class names are
  `AppleH9CamIn`, `AppleH10CamIn` and `AppleH13CamIn`. There is no `AppleH6CamIn` and no `AppleH16` in
  this kernel, so publishing `AppleH6CamIn` collides with nothing and touches no real camera class;
  `H6ISP.mediacapture` is the last entry of the vendor's table, which is why the plug-in is named that
  first. Whether this build's table stops earlier, on `H9ISP` or `H13ISP`, is what the first boot
  answers, and the plug-in is therefore delivered under every candidate name and logs the one that was
  opened.
- Our ADT (`scripts/regression-baseline-adt.adt`) already has a `product` node, with an `audio` child,
  so `/product/camera` is added as a child of an existing node and the `flash` property is set on the
  node the tree already has when there is one.

Three parts, all of them this repository's existing mechanisms rather than a new one:

1. **Kernel** (`DbyTorchIos.c`, `DbyTorchIos.cpp`, this directory): registers `AppleH6CamIn` and
   `DbyTorchUserClient` late, through `OSMetaClass::preModLoad`/`postModLoad` the way
   `drivers/NativeLegacyDisplay/LegacyDisplayProbe.cpp` does, publishes the service on the resource
   service, and puts `flash` on `/product/camera` — but only once the driver answers and the plug-in
   is visible, so nothing is published that has no LED behind it. `dby_torch_start` makes the plan;
   the commit runs just before launchd, after the mount. The user client accepts one scalar (0..1000)
   from `mediaserverd` only and switches the LED off when that process closes it.
2. **The mount** (`patches/xnu/zze-dby-torch-media-mount.patch`, `series-torch`): a fifth
   `native_gpu_addon_mount()` in `native_gpu_addon_prepare()` — the same call the Metal bundle, the
   LaunchDaemons, the wallpaper and the audio HAL plug-in use — union-mounting
   `/private/var/ngpu/mediacapture` over `/System/Library/MediaCapture`, with
   `native_gpu_addon5_stage/status` as its ladder and `dby_torch_ios_commit(stage == 5)` as its verdict,
   so nothing is published that the mount has not proven. The two name arguments come from the mother's
   overlay and her measurement, not from a guess: `refer/Pagani/patches/xnu/phone-torch-overlay.patch`
   passes `visible "/System/Library/MediaCapture"` and `own
   "/System/Library/MediaCapture/H6ISP.mediacapture"`, because her root *"has no entry there to prove
   the lower layer with, so the directory itself stands for it"* — a union over an empty sealed
   directory shows that directory and only the overlay can supply the file, and `native_gpu_addon_mount`
   does plain `vnode_lookup`s at stages 4 and 5, so a directory is a legal sentinel. The window is this
   tree's own arithmetic: `kern_exec.c:8193` allocates the scratch as one `map_page_size` (16 KiB), the
   four mounts use `0x400`, `0x600`, `0x800` and `0xa00`, a mount consumes its window plus `0x130`, and
   `0xc00` belongs to the `mkdir` and `unlink` paths that run after the mounts, so `0xe00` is the first
   window nothing else claims. The ladder is also the instrument that answers a refusal, and the two
   counters join the BootLog `A` row as `mount5=stage/status` the way `mount`, `mount2` and `mount3` are
   already read there: `1` with a status is no `/private/var/ngpu/mediacapture` (a delivery problem),
   `2` or `3` a refused mount, `4` that `/System/Library/MediaCapture` does not answer a lookup on this
   root, `5` that the overlay took. This is the same bindfs union the wallpaper, the LaunchDaemons and
   the audio HAL plug-in use; the root itself is never written.
3. **The plug-in** (`torch/DbyTorchPlugin.m`, `scripts/build-dby-torch-plugin.py`,
   `scripts/add-dby-torch-data.py`): a `dlopen`-ed bundle whose only job is to answer the vendor's
   flashlight messages. It exports no `CaptureDeviceCreate`, so no capture device appears and no
   camera appears anywhere; it registers a client only for the description `FigFlashlight`, and that
   stream's `TorchLevel` is what reaches the user client. It is delivered through the Data seed and
   ad-hoc signed, and its CDHash goes into the owner trust bundle, exactly as `ShengAudio.driver`
   does.

What this does **not** do, said plainly because the owner asked for the camera too: it makes the
flashlight a native iOS control, not the camera. A camera needs the SM8250 titan ISP pipeline —
CSIPHY, CSID, IFE, SMMU and the sensor's own initialization — and no part of that exists in this
repository yet; the mother's own camera half synthesises a test pattern inside mediaserverd and
records that it does not produce frames from the sensor. That is `task 212` and a separate round.

Deviation from the mother, on purpose: every log line here is `kprintf` and no `IOLog`, because this
board's COM4 mirrors `kprintf` rows only (`DbyTorch.cpp:77`, and the archive gate fails an object that
imports `_IOLog`).

## Building and testing

- `scripts/check-dby-torch-host.py` is the whole host check, in one command from the device
  repository root; `--tree` is the assembled build tree the kernel step works in, so the glue is
  compiled against the same XNU records the build uses:

  ```
  XNU_TOOLCHAIN_BIN=<toolchain>/CommandLineTools/usr/bin PATH=<llvm20-objcopy-shim>:$PATH \
    python3 scripts/check-dby-torch-host.py --tree <unixnu-core>/out/<device>/<os-build>/<board>/tree
  ```

  Without `--tree` it is the portable half: the three `.c` sources compiled strictly, then
  `Test/spmi.c`, `Test/core.c` and `Test/ios.c` under ASan/UBSan, then the `args` check (every boot
  argument the glue reads is in the table above and in `DbyTorch.h`, and they name no other). With
  `--tree` it also builds the kernel archive, so nothing about the glue is left uncompiled, and
  replays `patches/xnu/zze-dby-torch-media-mount.patch` after the whole patch series that build
  recorded (`xnu-inputs.json` sits next to the `tree` directory), which is how the ordering
  requirement of part 2 is checked rather than only stated. The glue
  half needs the two environment knobs the kernel step itself uses, and no others:
  `XNU_TOOLCHAIN_BIN`, which is how scripts/linux's `xcrun` shim reaches the Mach-O `ar` (a GNU index
  of a Mach-O archive lists no symbols, so ld64 would pull nothing and the torch would be silently
  absent), and the LLVM 20 `llvm-objcopy` shim first in `PATH`, because staging the class initializer
  is Mach-O section surgery. Do not put that toolchain bin first in `PATH` instead of naming it: its
  `as` is Apple's, and the host half is GCC against glibc. Measured 2026-11-09:

  ```
  PASS strict compile: 3 portable sources (-Wconversion -Wshadow -Wmissing-prototypes)
  PASS spmi: 9045 checks (…)
  PASS core: 1580100 checks (…)
  PASS ios: 19 checks (…)
  PASS args: 5 boot arguments read, each in README.md and DbyTorch.h
  PASS: 5 objects in /tmp/…/kernel/libDbyTorch.a (25 entry points, 239 kernel imports, every one defined by <tree>/build/xnu/ios-obj/mach.development.bcm2837)
  PASS mount patch: 7 tests against …/dby_w09/xnu-inputs.json
  PASS DbyTorch host checks
  ```

- `scripts/build-dby-torch.py` is what that last line runs, and it is the mechanism this board
  already uses for `package/DbyTouch` (`scripts/build-dby-touch.py`): it replays the tree's own XNU
  compile records instead of guessing an ABI. `iokit/DEVELOPMENT/IOPlatformExpert.cpo.json` gives the
  C++ flags and the header search path behind `IOLib.h`; `bsd/DEVELOPMENT/bsd_init.o.json` gives the
  C ones. Every `-W` of the record is dropped and replaced by `-Wall -Wextra -Werror` plus the
  demotions below; the records are read only and the objects go to `--out`, so a check build leaves
  the tree as it is. Both records exist in the tree above (887 `.o.json` files there), which is why
  the glue needs no standalone SDK.
- What the archive line proves, per object: zero warnings; the 25 entry points strong; the 239
  imports — 26 C names, each on the allowlist and no others, and 213 `__Z…` IOKit and libkern names —
  each one defined by this tree's own linked kernel, which is the only thing that can say the
  mangled half will link; `_kprintf` present and `_IOLog` absent, because this board's COM4 mirrors
  `kprintf` rows only; no `__DATA,__mod_init_func` in the four sources that keep theirs, so nothing
  registers itself at boot, and the one initializer `DbyTorchIos.cpp` needs is moved out of that
  section by `llvm-objcopy --remove-section=… --globalize-symbol=__GLOBAL__sub_I_DbyTorchIos.cpp` —
  the recipe proves the surgery touched nothing but that section and those symbols by comparing the
  two objects' `__text` bytes and re-reading their sections; ARM64 Mach-O object with
  `LC_BUILD_VERSION` iOS 17.0; `-mgeneral-regs-only`, so no floating point in kernel mode.
- Two upstream header defects are why the warning list is not bare `-Werror`, recorded rather than
  hidden: `osfmk/vm/vm_kern.h:793` and `:841` (reachable from `IOKit/system.h`) `|` a `kmr_flags_t`
  with a `kmem_flags_t`, which clang diagnoses in C++, and the record's `-Weverything` plus
  `IOLib.h:330/336`, `OSValueObject.h:471/486`, `kalloc.h:1549` trip the four demotions
  `build-dby-touch.py` already records for this board (`-Wno-missing-designated-field-initializers`,
  `-Wno-c99-designator`, `-Wno-reorder-init-list`, `-Wno-unnecessary-virtual-specifier`). None of
  them is the torch's own text.
- Still open, in this order: the mount patch and its `series-torch` (part 2 above), the plug-in and its
  Data-seed delivery (part 3), the 16 bytes the two native arguments cost against the 11 free on the
  command line (`## Boot arguments`), and the USB console that has never enumerated since the argument
  swap — until that answers, a `[torch]` row cannot be read and every torch round is a photograph.

Host checks do not establish that the LED lights on the device.
