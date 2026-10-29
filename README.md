# UniXNU on the Huawei MatePad 11 (dby_w09)

Device repository for the **DBY-W09** (Huawei MatePad 11, Qualcomm SM8250 / kona, 6 GiB RAM,
128 GiB UFS). It carries the board description, the board device tree and XNU patch order, the
kernel defconfig, the board package and build scripts, the firmware selection, the partition
layout and the porting record. Shared XNU, driver and kit implementations come from
[UniXNU](https://github.com/UniXNU/UniXNU); the SM8250 hardware description and the UFS driver
come from the two feeds pinned in `feeds.toml`.

Target software: iPadOS 17.7.11 (build 21H461) with the donor identity `iPad7,1` / `j120ap`, the
same verified combination as the official [sheng](https://github.com/UniXNU/UniXNU-sheng) port.
Chip ID `0x8011`, board ID `12`.

- [Documentation](https://unixnu.com) · [Porting](https://unixnu.com/porting/) ·
  [Building](https://unixnu.com/building) · [Device record](porting.toml)

## Layout

```text
board.toml                  schema 2: board, SoC feed, OS profile, [components], firmware, payload
feeds.toml                  this board's git pins: the sm8250 SoC feed and the ShengUfs driver
unixnu.lock                 resolved git sources, written by `unixnu fetch`
dts/dby_w09.dts             board device tree; the SoC feed supplies dts/sm8250.dtsi
patches/xnu/series          board XNU patch order
configs/dby_w09_defconfig   kernel configuration (owner Data volume UUID is compiled in)
configs/dby_w09_abiaudit_defconfig
                            variant used to record the storage ABI bindings
package/                    board C/C++ sources built into the kernel (boot progress, probes, policy)
scripts/                    board build recipes, ADT generator, XNU patchers, checks
proprietary-files.txt       Apple inputs and their hashes; never committed
local.toml.example          machine-specific source overrides (copy to the ignored local.toml)
porting.toml                porting record: areas, statuses and evidence
```

`package/` appears in the assembled tree as `kits/dby_w09`.

## Prerequisites

1. **UniXNU** checkout plus an arm64 Apple toolchain (clang/lld, `xcrun` shims) on the build host.
   `unixnu` needs Python 3.11 or newer (`tomllib`).
2. **UEFI firmware**: a Mu-Silicium checkout exposing `Platforms/Huawei/dby_w09Pkg/dby_w09.dsc`,
   passed with `--uefi` (or `UNIXNU_UEFI`). `scripts/build-boot-progress.py` needs it for the
   panic-screen glyph table, so it is a build input of the kernel step, not only of the firmware.
3. **iPadOS restore image** `iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw`, from the URL in
   `board.toml [ipsw]`; `./unixnu ipsw fetch` extracts and verifies the members against
   `[ipsw.sha256]`. Apple content never enters Git.
4. **A scratch Data volume seed** for the writable `/private/var`. It must be produced on macOS
   (`prepare-owner-data-fixture.py` uses `hdiutil`); see
   [userdata](https://unixnu.com/guide/userdata). Its APFS volume UUID has to equal
   `CONFIG_XNU_OWNER_DATA_VOLUME_UUID` in the defconfig, otherwise the kernel refuses the mount
   (`offline scratch Data mount failed: 2`). Growing an existing seed keeps the UUID; a fresh
   fixture does not and therefore needs a kernel rebuild.
5. **Trust cache**: the unmodified original static + System bundle, built by
   `scripts/build-original-system-trust.py` and passed with `--trust-cache`.

## Build

```sh
cd UniXNU
./unixnu defconfig dby_w09_defconfig -D ../UniXNU-dby_w09 --uefi ../uefi-sheng
./unixnu fetch    -D ../UniXNU-dby_w09
./unixnu build kernel  -D ../UniXNU-dby_w09 --jobs 4
./unixnu build kernel  -D ../UniXNU-dby_w09 --jobs 4
./unixnu build payload -D ../UniXNU-dby_w09 --jobs 4 \
  --unique-chip-id <platform seed for the device tree> \
  --trust-cache build/original-static-system-trust/original-static-system.bundle \
  --data-ramdisk <data-final.dbr>
```

Two things about that sequence are load-bearing:

- **`defconfig` is not optional.** The kernel build reads only `out/dby_w09/.config`, so editing
  `configs/dby_w09_defconfig` has no effect until `defconfig` regenerates it. A defconfig-only
  change looks like it built and then fails at runtime.
- **`build kernel` runs twice.** `scripts/build-platform-power.py` links a no-op `sheng_pmgr_run`
  stub on its first pass and prints `Build the kernel again to link the bridge.`; only the second
  pass compiles the real bridge. The payload step refuses a kernel without `_sheng_pmgr_run`
  (`AMFI: No PMGR?!?`), so a single pass produces an image that cannot boot.

Kernel arguments reach the device through the payload's ESP `unixnu.toml`, so a
`[payload] kernel_args` change only needs a payload rebuild.

## Flash

```sh
fastboot flash boot    <firmware image>    # packed by the firmware tree's own script
fastboot flash preload <payload.img>       # ~208 MiB
```

`preload` carries the payload because `cache` is 100 MiB and the prelinked kernel is 119 MiB.
Never re-image `userdata`: on this device it holds the iPadOS root volume the kernel mounts.

`unixnu bootimg` packs this board's image correctly but `bootimg.verify()` reads the v0/v1 header
fields, so any `header_version >= 3` board rejects its own output; the firmware image is packed by
the firmware tree instead (see Deviations).

## Status

Measured on hardware, October 2026 (payload `461538fa…`, kernel `de197e0d…`). See
`porting.toml` for the per-area record and evidence.

| Area | State |
| --- | --- |
| UEFI firmware, panel, keys, USB at boot | boots on hardware |
| Kernel boot to launchd | reaches user space; original iPadOS binaries exec against the unmodified AMFI |
| Root and data volumes | native UFS root, OS and SystemOS windows mounted, packed Data seed with a persistent overlay |
| Display | XNU presents scanout: the iPadOS lock screen is on the panel |
| Input | no XNU HID device yet, so the lock screen cannot be passed |
| GPU | not ported; the current frames are CPU-rendered scanout |
| Audio, network, camera | not started |

## Known firmware message

Before UniXNU starts, the panel shows `Error: Image at … start failed: Not Found` together with
`No handles survived filtering!` and `USB boot desired, but no USB devices found on first attempt`.
That is Mu's `MsBootPolicy.efi`: it is a BDS boot option (its module type is
`UefiApplicationEntryPoint`) whose entry point returns the boot-policy status, and Silicium's
`MsBootPolicyLibGetBootSequence` returns `EFI_UNSUPPORTED`, so Mu falls back to the hardcoded
HDD → USB → PXE scan, finds no USB disk and returns `EFI_NOT_FOUND`, which BdsDxe then reports.
It costs one 6-second retry and is unrelated to UniXNU; implementing that one library hook is the
only way to silence it.

## Deviations

Recorded so that a reviewer knows what is not yet publishable, rather than discovering it:

- **The `sm8250` feed pin is not yet traceable.** `feeds.toml` pins
  `FFT-infra/UniXNU-sm8250-common@6bb9b025…`, but the local copy's content does not match the
  revision recorded when that repository was last fetched (`26fd784e…`) and is missing its
  `LICENSE` and `kits/`. Until the board's SoC changes are committed on top of an upstream clone
  and the pin re-recorded, `unixnu fetch` cannot reproduce the SoC feed. The `ShengUfs` pin is
  clean: its local copy is byte-identical to the fetch at `ab540590…`.
- `unixnu build firmware` is not wired: the firmware tree is an offline Mu-Silicium derivative
  installed by its own script, so `[firmware]` records the intended paths and the build runs by hand.
- Storage ABI bindings are consumed from a recorded static table
  (`scripts/build-storage-bindings.py`); the live audit that would regenerate it on this board is
  not yet wired.
- The verified-root policy and probe inputs come from a macOS-only producer; the tree carries
  recorded copies rather than a reproducible build.
- `porting.toml` is validated by `scripts/porting-record.py` from a current UniXNU checkout, and
  `.github/workflows/porting-record.yml` calls the reusable workflow from `UniXNU/UniXNU`.

## License

BSD-2-Clause-Patent. See [LICENSE](LICENSE). Apple restore-image content, derived volume images and
owner signing keys are not part of this repository.
