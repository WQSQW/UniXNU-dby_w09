# DbyTorch

The DBY-W09 (Huawei MatePad 11, SM8250/kona) LED torch from XNU: PM8150L's flash LED module (SPMI
USID 5, block base `0xd300`, the 4-channel flash-v2 block; this unit's torch is on channel 0 only)
driven over the SM8250 v5 SPMI PMIC arbiter as apps EE 0, with a kernel API.

Status: the portable core passes its host checks and the glue compiles, with the kernel archive
gates, against a real tree's XNU compile records (see `## Building and testing`); it has not been
linked into a kernel, wired into the device repository or run on the device. Default off: without
the boot arguments nothing runs, maps, allocates or writes. The iOS-facing half — the Control Center
flashlight button reaching this driver — is open, and `## The flashlight of iOS` says what has to be
measured on this root before any of it is written.

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
- **Faults**: `STATUS1` (short/open), `STATUS2`'s OTST and VPH-droop bits and `INT_RT_STS`, polled
  every 100 ms while on and once immediately after switching on. Any fault, bus failure, timeout,
  refusal or failed read-back switches the LED off (`EN_LED_CTRL` and `MOD_CTRL`, the one write
  allowed after the bus closed) and keeps the driver off until reboot.
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

Byte counts on the command line, with the leading space: `dby-torch=2` 12, `dby-torch=1` 11,
`dby-torch=1 dby-torch-test=100` 33. The device repository's `kernel_args` budget is 1000 bytes for
the whole line, so a torch round has to count these against what it removes.

The first run on the device is `dby-torch=2` (a read-only take-over: it proves the windows, the
arbiter version, the APID, the write EE and the identity bytes and writes nothing), then
`dby-torch=1 dby-torch-test=100` (100 ms at full), then the API.

## Interfaces

- `dby_torch_set(level)` and `dby_torch_request(level, "who")` (`DbyTorch.h`): 0 off, 1..1000
  permille of the cap; any thread that may sleep; waits at most 200 ms for the driver's thread and
  returns what happened (`0`, `-19`, `-22`, `-5`, `-1`, `-110`).
- `dby_torch_level()`: the level now, or `-19` when the driver is not running.
- `dby_torch_status[16]`: the core's export (`DbyTorchCore.c`), documented in `DbyTorch.h`. Every
  step of the run is also a `[torch]` line on `kprintf`, which is what this board's COM4 mirrors.

## The flashlight of iOS

Open, deliberately not written yet. The phone's chain (Control Center's flashlight button →
`+[AVFlashlight hasFlashlight]` → `MGGetBoolAnswer("camera-flash")` → `FigFlashlight` in
mediaserverd → the capture vendor → a stream's `TorchLevel`) is what this driver has to be reached
through, and the mother in `camera-ref/pagani/PaganiTorch` documents one working shape of it. Its
three constants are **not** portable facts and will not be copied: the class the vendor looks for,
the MobileGestalt key and node on this OS version, and the media-capture plug-in name are per-build.
Before any of that is written, this root and its shared cache have to answer which of
`AppleH16/13/10/9/6CamIn` the vendor's table probes on this iPadOS build, whether `camera-flash`
resolves to the same node and property here, and which `.mediacapture` directories exist. Until that
measurement exists, the torch is a kernel API and nothing pretends otherwise.

## Building and testing

- `scripts/check-dby-torch-host.py` is the whole host check, in one command from the device
  repository root; `--tree` is the assembled build tree the kernel step works in, so the glue is
  compiled against the same XNU records the build uses:

  ```
  python3 scripts/check-dby-torch-host.py --tree <unixnu-core>/out/<device>/<os-build>/<board>/tree
  ```

  Without `--tree` it is the portable half: the two `.c` sources compiled strictly, then
  `Test/spmi.c` and `Test/core.c` under ASan/UBSan, then the `args` check (every boot argument the
  glue reads is in the table above and in `DbyTorch.h`, and they name no other). With `--tree` it
  also builds the kernel archive, so nothing about the glue is left uncompiled. Measured 2026-11-09:

  ```
  PASS strict compile: 2 portable sources (-Wconversion -Wshadow -Wmissing-prototypes)
  PASS spmi: 9045 checks (…)
  PASS core: 1580100 checks (…)
  PASS args: 4 boot arguments read, each in README.md and DbyTorch.h
  PASS: 3 objects in /tmp/…/kernel/libDbyTorch.a (18 entry points, 22 kernel imports)
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
- What the archive line proves, per object: zero warnings; the 18 entry points strong; the 22 kernel
  imports each on the allowlist and no others — `_kprintf` present and `_IOLog` absent, because this
  board's COM4 mirrors `kprintf` rows only — the C++ ones (`__Z…`) are IOKit/libkern's own; no
  `__DATA,__mod_init_func` anywhere, so nothing registers itself at boot; ARM64 Mach-O object with
  `LC_BUILD_VERSION` iOS 17.0; `-mgeneral-regs-only`, so no floating point in kernel mode.
- Two upstream header defects are why the warning list is not bare `-Werror`, recorded rather than
  hidden: `osfmk/vm/vm_kern.h:793` and `:841` (reachable from `IOKit/system.h`) `|` a `kmr_flags_t`
  with a `kmem_flags_t`, which clang diagnoses in C++, and the record's `-Weverything` plus
  `IOLib.h:330/336`, `OSValueObject.h:471/486`, `kalloc.h:1549` trip the four demotions
  `build-dby-touch.py` already records for this board (`-Wno-missing-designated-field-initializers`,
  `-Wno-c99-designator`, `-Wno-reorder-init-list`, `-Wno-unnecessary-virtual-specifier`). None of
  them is the torch's own text.
- Still open, in this order: wiring into the device repository (`package/DbyTorch/`, the `archives`
  entry next to the touch recipe, `defconfig`, `kernel_args` inside the 1000-byte budget), the two
  boot rounds of `## Boot arguments`, then the plug-in for `## The flashlight of iOS`.

Host checks do not establish that the LED lights on the device.
