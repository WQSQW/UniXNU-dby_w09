# Agent instructions

This repository owns the dby_w09 (Huawei MatePad 11, SM8250 / kona) board configuration, kernel
defconfig, board package, board scripts, firmware selection, partition layout and the porting
record. Shared XNU, driver and kit implementations belong to `UniXNU`; the SM8250 driver sources
this board currently builds from are part of that tree, not of this repository.

Read before editing:

| Task | Required reading |
| --- | --- |
| Every port | [Porting](https://unixnu.com/porting/), [device repository](https://unixnu.com/porting/device-repository), [component catalog](https://unixnu.com/reference/components) |
| Kernel | Selected OS profile (iPadOS 17.7.11 / 21H461, donor iPad7,1), XNU source, [prelink](https://unixnu.com/concepts/prelink) |
| Storage | [UFS](https://unixnu.com/porting/ufs), [userdata and root windows](https://unixnu.com/guide/userdata) |
| Display | [Bring-up](https://unixnu.com/porting/bring-up), legacy display and GOP handoff notes in [debugging](https://unixnu.com/guide/debugging) |
| Input | [Input](https://unixnu.com/porting/input) before adding any HID or button code |
| Images | [Payload](https://unixnu.com/concepts/payload), [UEFI firmware](https://unixnu.com/porting/uefi-firmware), [Building](https://unixnu.com/building) |
| Record | This file's `porting.toml` and [records](https://unixnu.com/porting/records) |

Rules this board has already learned the hard way:

- Never hardcode a hardware value. Read it from the device, a backup of its partition table or an
  official record, and write the source next to the value. `userdata` first/last LBA came from a
  full 9008 backup of the GPT; `owned_memory_size` came from the same-SoC official board.
- Prefer the official mechanism over a local invention. Every homebrew step in `scripts/` and
  `package/` is a suspect: when a checkpoint fails, first check whether upstream already provides
  the missing piece (or has superseded ours) before adding another workaround.
- Kernel arguments reach the device only through the payload (the ESP `unixnu.toml` of the built
  image). Changing `board.toml [payload] kernel_args` costs a payload rebuild, not a kernel rebuild.
- The `userdata` partition holds the tablet's own iPadOS root volume. Never image it, and never
  enable a boot argument that writes logs there.
- Apple restore-image members, the Data seed and any owner private key stay out of Git. See
  `proprietary-files.txt`.

Preserve upstream names: source file names, runtime classes, bundle identifiers, symbols and
protocol GUIDs. Do not rename an official component to a board-local name.

Run from the UniXNU checkout:

```sh
./unixnu defconfig dby_w09_defconfig -D ../UniXNU-dby_w09
./unixnu status --json -D ../UniXNU-dby_w09
./unixnu tree -D ../UniXNU-dby_w09
./unixnu build kernel -j 2 -D ../UniXNU-dby_w09 --uefi ../uefi-sheng
./unixnu build payload -D ../UniXNU-dby_w09 --uefi ../uefi-sheng <inputs from README.md>
python3 scripts/porting-record.py check --device ../UniXNU-dby_w09
```

`unixnu defconfig` must run after any `configs/*_defconfig` edit: the generated `.config`, not the
defconfig text, is what the kernel build reads.

Update the affected `porting.toml` area's findings or evidence with every code change, and report
commands, outputs, missing inputs and hardware observations separately. Write concise English here.
