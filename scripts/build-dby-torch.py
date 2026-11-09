#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Build build/dby-torch/libDbyTorch.a (the DBY-W09 LED torch) with this tree's actual XNU compile records.

Run by the UniXNU kernel step as an archive recipe (board.toml [kernel] archives, cwd = the tree), the slot
scripts/build-dby-touch.py also occupies. The archive holds the portable core (DbyTorchSpmi.c, DbyTorchCore.c,
DbyTorchIos.c with the BSD record's C flags) and the kernel glue DbyTorch.cpp and DbyTorchIos.cpp (with the IOKit
C++ record's flags). It is linked through CONFIG_XNU_EXTRA_LD_ARCHIVES, not force_load: package/BootProgress.c
names dby_torch_start(), so the plain archive path always pulls it. Kernel code here never uses floating point
(-mgeneral-regs-only). Only DbyTorchIos.cpp may carry a static initializer, and its initializer is taken out of
__DATA,__mod_init_func by llvm-objcopy and globalized as __GLOBAL__sub_I_DbyTorchIos.cpp, so the class is
registered when dby_torch_ios_commit() asks for it and not at boot: a kernel without dby-ti=1 gets neither
the AppleH6CamIn class nor the service. That is the same late registration drivers/NativeLegacyDisplay uses. The
objcopy is checked, not trusted: the __text bytes must be the ones the compiler produced and the section must be
gone, because a renamed text section would change which instructions run. The other objects must carry no
__mod_init_func at all: the driver itself has no class, and its text would otherwise sit in the boot of a kernel
that was never asked to run it (it stays inert unless the payload carries dby-torch=<mode>).

The records are read, never written. Recorded commands carry no -Werror of their own and their -W set is XNU's
(-Weverything included), so every -W is dropped and this script's list is used instead: -Wall -Wextra -Werror plus
five demotions, four of them verbatim from build-dby-touch.py because XNU's own OSMetaClass/KALLOC_TYPE_DEFINE
macros trip them in every driver that includes IOLib.h, and the fifth because osfmk/vm/vm_kern.h:793 and :841 (from
IOKit/system.h) | a kmr_flags_t with a kmem_flags_t, which clang diagnoses in C++ and only kernel-owned text could
fix. Both reasons are written in README.md, not hidden in a flag list.

The gate after the archive is what a kernel build cannot say: every entry point REQUIRED names really defined,
every kernel import on the allowlist and nothing else (_kprintf present and _IOLog absent, because this board's
COM4 mirrors kprintf rows only), every import - the IOKit and libkern mangled names the allowlist cannot speak for -
defined by this tree's own linked kernel, no __mod_init_func outside the staged object, and each object audited as
an ARM64 Mach-O for iOS 17.0.

Source lookup order is tree/drivers/DbyTorch, tree/kits/dby_w09/DbyTorch, package/DbyTorch - kits/dby_w09 is this
repository's package/, so the in-repo sources are found with no copy step.

  build-dby-torch.py [--tree TREE] [--source-dir DIR] [--out DIR]
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARCH = 'DEVELOPMENT_ARM64_BCM2837'
RECORD_DIR = Path(f'build/xnu/ios-obj/{ARCH}')
KERNEL_IMAGE = Path('build/xnu/ios-obj/mach.development.bcm2837')
CXX_RECORD = RECORD_DIR / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
C_RECORD = RECORD_DIR / 'bsd/DEVELOPMENT/bsd_init.o.json'
C_SOURCES = ['DbyTorchSpmi.c', 'DbyTorchCore.c', 'DbyTorchIos.c']
CXX_SOURCES = ['DbyTorch.cpp']
STAGED = 'DbyTorchIos.cpp'
INITIALIZER = '__GLOBAL__sub_I_DbyTorchIos.cpp'
HEADERS = ['DbyTorchRegs.h', 'DbyTorchText.h', 'DbyTorchSpmi.h', 'DbyTorchCore.h', 'DbyTorchIos.h', 'DbyTorch.h']
WARNINGS = ['-Wall', '-Werror', '-Wextra',
            '-Wno-missing-designated-field-initializers', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-unnecessary-virtual-specifier',
            # osfmk/vm/vm_kern.h:793 and :841, through IOKit/system.h, which every IOLib.h user gets.
            '-Wno-deprecated-enum-enum-conversion']
# Everything the kernel may take from this archive; the C++ IOKit/libkern names (__Z...) are allowed by shape.
REQUIRED = ['_dby_torch_start', '_dby_torch_status', '_dby_torch_set', '_dby_torch_request', '_dby_torch_level',
            '_dbt_core_init', '_dbt_core_set', '_dbt_core_poll', '_dbt_core_export', '_dbt_config_make', '_dbt_test_ms',
            '_dbt_spmi_discover', '_dbt_spmi_open', '_dbt_spmi_read', '_dbt_spmi_write', '_dbt_spmi_emergency_stop',
            '_dbt_current_law', '_dbt_level_ma',
            '_dbt_ios_plan', '_dbt_ios_final', '_dbt_ios_level',
            '_dby_torch_ios_plan', '_dby_torch_ios_commit', '_dby_torch_ios_status', INITIALIZER]
KERNEL_IMPORTS = {'_kprintf', '_IODelay', '_IOSleep', '_IOLockAlloc', '_IOLockLock', '_IOLockUnlock',
                  '_IOMalloc_internal', '_KHEAP_DEFAULT', '_PE_parse_boot_argn', '_absolutetime_to_nanoseconds',
                  '_mach_absolute_time', '_assert_wait_timeout', '_clear_wait', '_current_thread', '_thread_block',
                  '_thread_wakeup_prim', '_kernel_thread_start', '_thread_deallocate', '_thread_set_thread_name',
                  '_memcpy', '_memset', '_bzero', '_lck_mtx_lock', '_lck_mtx_unlock', '___stack_chk_fail',
                  '___stack_chk_guard',
                  # the iOS half: the caller's process name for the mediaserverd gate, the kmod_info the late
                  # OSMetaClass registration names, and the IODeviceTree plane the /product/camera node joins.
                  '_proc_selfname', '_g_kernel_kmod_info', '_gIODTPlane',
                  # what clang emits for new/delete of an IOObject: defined in this tree's own kernel
                  # (mach.development.bcm2837 has both at S), and the same pair the accepted libShengHidInput.a,
                  # libNativeLegacyDisplay.a and libDbyTouch.a already take.
                  '_OSObject_typed_operator_new', '_OSObject_typed_operator_delete'}


def fail(message):
    raise SystemExit('build-dby-torch: ' + message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load_record(tree, relative):
    path = tree / relative
    if not path.is_file():
        fail(f'{path} missing; the kernel build leaves the compile records this recipe replays, so build the kernel once first')
    return json.loads(path.read_text().strip().rstrip(','))


def command(record, source, obj, extra):
    """The record's command for another source and object: its ABI flags and include path, without its warning or dependency options."""
    arguments, result, i, skipped = record['arguments'], [], 0, []
    while i < len(arguments):
        value = arguments[i]
        if value == record['file']:
            result.append(str(source))
        elif value == '-I' and i + 1 < len(arguments):
            # A pruned tree no longer carries every include directory the kernel had (EXPORT_HDRS/libsa is the
            # usual one) and -Werror turns -Wmissing-include-dirs into a failure. Same handling as build-dby-touch.py.
            directory = arguments[i + 1]
            if Path(directory).is_dir():
                result += ['-I', directory]
            else:
                skipped.append(directory)
            i += 1
        elif value == '-o':
            result += ['-o', str(obj)]
            i += 1
        elif value in ('-MF', '-MJ', '-MT', '-MQ'):
            i += 1
        elif value == '-Xclang' and i + 1 < len(arguments) and arguments[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP', '-MMD'):
            result.append(value)
        i += 1
    return result + extra, skipped


def sections(obj):
    """{(segment, section): (fileoff, size)} of an MH_OBJECT's one LC_SEGMENT_64."""
    data = obj.read_bytes()
    count = struct.unpack_from('<I', data, 16)[0]
    offset, result = 32, {}
    for _ in range(count):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x19:
            nsects = struct.unpack_from('<I', data, offset + 64)[0]
            for k in range(nsects):
                at = offset + 72 + 80 * k
                sect = data[at:at + 16].rstrip(b'\0').decode()
                seg = data[at + 16:at + 32].rstrip(b'\0').decode()
                _, length, fileoff = struct.unpack_from('<QQI', data, at + 32)
                result[(seg, sect)] = (fileoff, length)
        offset += size
    return result


def text(obj):
    """The __text bytes: instructions only, so an objcopy that only touched symbols and one section is caught by
    anything that moved or changed a single instruction."""
    fileoff, length = sections(obj)[('__TEXT', '__text')]
    return obj.read_bytes()[fileoff:fileoff + length]


def audit(obj):
    """ARM64 Mach-O object for iOS, minimum 17.0."""
    data = obj.read_bytes()
    magic, cpu, _, kind, count, _ = struct.unpack_from('<6I', data)
    if (magic, cpu, kind) != (0xfeedfacf, 0x100000c, 1):
        fail(f'{obj}: not an ARM64 Mach-O object')
    offset, version = 32, None
    for _ in range(count):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x32:
            version = struct.unpack_from('<II', data, offset + 8)
        offset += size
    if version != (2, 0x110000):
        fail(f'{obj}: LC_BUILD_VERSION {version} is not iOS 17.0')


def compile_one(record, source, obj, extra):
    obj.parent.mkdir(parents=True, exist_ok=True)
    obj.unlink(missing_ok=True)
    arguments, skipped = command(record, source, obj, extra)
    (obj.parent / f'compile-command-{obj.stem}.json').write_text(json.dumps(arguments, indent=2) + '\n')
    result = subprocess.run(arguments, cwd=record['directory'], capture_output=True, text=True, timeout=900)
    output = (result.stdout + result.stderr).strip()
    if result.returncode:
        fail(f'{source}:\n{output[-6000:]}')
    if 'warning:' in output:
        fail(f'{source} compiled with warnings:\n{output[-6000:]}')
    audit(obj)
    return skipped


def tool(name):
    found = shutil.which(name)
    if not found:
        fail(f'{name} not on PATH; the kernel step exports the LLVM prefix that speaks Mach-O')
    return found


def symbols(archive):
    """Defined and undefined external names, from llvm-nm --extern-only.

    Two shapes, not one: a defined symbol is "<address> <kind> <name>", an undefined one is "<kind> <name>" with the
    address column all spaces, so the line is split rather than matched against an address."""
    output = subprocess.run([tool('llvm-nm'), '--extern-only', str(archive)], check=True, capture_output=True,
                            text=True).stdout
    defined, undefined = set(), set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 2:
            kind, name = fields
        elif len(fields) == 3:
            _, kind, name = fields
        else:
            continue
        (undefined if kind in 'Uu' else defined).add(name)
    return defined, undefined - defined


def kernel_symbols(kernel):
    """Every name the tree's own linked kernel defines, or None when that kernel is not there yet (a first build in a fresh tree).

    This is what makes the C++ half's imports a fact instead of a shape: of the archive's 239 external names, 213 are IOKit and libkern
    mangled symbols (__Z...) that no allowlist can say anything about, and the only thing that says this kernel really carries them is
    this kernel."""
    if not kernel.is_file():
        return None
    output = subprocess.run([tool('llvm-nm'), '--defined-only', str(kernel)], check=True, capture_output=True,
                            text=True, timeout=900).stdout
    return {line.split()[-1] for line in output.splitlines() if line.split()}


def symdef(archive):
    """The name of the archive's first member, which is where its symbol index sits.

    This is a gate and not a detail: `xcrun ar` execs whatever `ar` the environment resolves, and on this host that is GNU ar unless the
    kernel step's toolchain bin comes first. A GNU index of a Mach-O archive lists none of its symbols, so ld64 would find nothing to pull
    and the undefined `_dby_torch_start` in BootProgress.o would fail the link (or, worse, the torch would be silently absent)."""
    data = archive.read_bytes()
    if not data.startswith(b'!<arch>\n'):
        fail(f'{archive}: not an ar archive')
    name = data[8:24].strip()
    if name.startswith(b'#1/'):
        length = int(name[3:])
        return data[68:68 + length].rstrip(b'\0').decode()
    return name.decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path,
                        default=Path(os.environ['UNIXNU_TREE']) if os.environ.get('UNIXNU_TREE') else None,
                        help="assembled kernel tree; `unixnu` runs a recipe from the tree, so the default is this "
                             "script's own tree root or $UNIXNU_TREE")
    parser.add_argument('--source-dir', type=Path, help='DbyTorch sources; defaults to the lookup order')
    parser.add_argument('--out', type=Path, help='output directory (default: TREE/build/dby-torch)')
    args = parser.parse_args()
    tree = (args.tree or ROOT).resolve()
    if not (tree / RECORD_DIR).is_dir():
        fail(f'{tree} has no {RECORD_DIR}: point --tree at the assembled tree')
    candidates = [args.source_dir] if args.source_dir else [tree / 'drivers/DbyTorch', tree / 'kits/dby_w09/DbyTorch',
                                                            ROOT / 'package/DbyTorch']
    source = next((c.resolve() for c in candidates if c and (c / 'DbyTorch.cpp').is_file()), None)
    if source is None:
        fail('DbyTorch.cpp not found in: ' + ', '.join(str(c) for c in candidates))
    missing = [name for name in C_SOURCES + CXX_SOURCES + [STAGED] + HEADERS if not (source / name).is_file()]
    if missing:
        fail('missing sources: ' + ', '.join(missing))
    out = (args.out or tree / 'build/dby-torch').resolve()
    shutil.rmtree(out / 'obj', ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    cxx_record, c_record = load_record(tree, CXX_RECORD), load_record(tree, C_RECORD)
    extra = ['-I', str(source), '-I', str(tree / '.cache/xnu-work/osfmk'), '-mgeneral-regs-only'] + WARNINGS
    skipped, objects = [], []

    def object_name(name):
        """The whole file name, dots and all: DbyTorchIos.c and DbyTorchIos.cpp share a stem, and one stem would mean
        one object silently replacing the other before the archive is written."""
        return name.replace('.', '_') + '.torch.o'

    for name in C_SOURCES:
        obj = out / 'obj' / object_name(name)
        skipped += compile_one(c_record, source / name, obj, extra)
        objects.append(obj)
    for name in CXX_SOURCES:
        obj = out / 'obj' / object_name(name)
        skipped += compile_one(cxx_record, source / name, obj, extra)
        objects.append(obj)
    for obj in objects:
        if any(sect == '__mod_init_func' for _, sect in sections(obj)):
            fail(f'{obj.name} has a static initializer; only {STAGED} may, and it is registered late')
    staged = out / 'obj' / object_name(STAGED)
    skipped += compile_one(cxx_record, source / STAGED, staged, extra)
    late = out / 'obj' / object_name(STAGED).replace('.torch.o', '.torch.late.o')
    late.unlink(missing_ok=True)
    subprocess.run([tool('llvm-objcopy'), '--remove-section=__DATA,__mod_init_func',
                    '--globalize-symbol=' + INITIALIZER, str(staged), str(late)], check=True, timeout=300)
    if text(staged) != text(late) or any(sect == '__mod_init_func' for _, sect in sections(late)):
        fail('staging the class initializer changed its instructions or kept __mod_init_func')
    objects.append(late)
    archive = out / 'libDbyTorch.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True, timeout=300)
    index = symdef(archive)
    if not index.startswith('__.SYMDEF'):
        fail(f'{archive}: symbol index is {index!r}, not __.SYMDEF; `xcrun ar` execs whatever `ar` the environment resolves, and on this '
             "host that is GNU ar unless the Mach-O toolchain bin is reachable (the kernel step gets it from PATH; scripts/linux's xcrun "
             "shim calls it XNU_TOOLCHAIN_BIN). A GNU index of a Mach-O archive lists none of its symbols, so ld64 would pull nothing and "
             "the kernel's undefined _dby_torch_start would fail the link or, worse, the torch would be silently absent")
    defined, undefined = symbols(archive)
    problems = [f'{name} not defined' for name in REQUIRED if name not in defined]
    unexpected = sorted(name for name in undefined if name not in KERNEL_IMPORTS and not name.startswith('__Z'))
    problems += [f'unexpected kernel import {name}' for name in unexpected]
    if problems:
        fail('archive symbol check failed:\n  ' + '\n  '.join(problems))
    kernel = tree / KERNEL_IMAGE
    table = kernel_symbols(kernel)
    if table is None:
        print(f"note: {kernel} is not here, so the imports were checked against the allowlist and the __Z shape only")
    else:
        absent = sorted(name for name in undefined if name not in table)
        if absent:
            fail(f'{kernel} does not define {len(absent)} import(s): ' + ', '.join(absent[:12]))
    listed = [source / name for name in C_SOURCES + CXX_SOURCES + [STAGED] + HEADERS] + [Path(__file__).resolve()]
    report = {'tree': str(tree), 'source_dir': str(source), 'archive': str(archive), 'archive_bytes': archive.stat().st_size,
              'archive_sha256': sha(archive), 'archive_index': index,
              'objects': {obj.name: obj.stat().st_size for obj in objects},
              'source_sha256': {p.name: sha(p) for p in listed}, 'warning_flags': WARNINGS,
              'entry_points': REQUIRED, 'undefined': sorted(undefined), 'skipped_include_dirs': skipped,
              'kernel_image': str(kernel), 'kernel_imports_checked_against_kernel': table is not None,
              'hardware_execution': False}
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    if skipped:
        print('note: skipped missing include dirs: ' + ', '.join(sorted(set(skipped))))
    checked = f', every one defined by {kernel}' if table is not None else ''
    print(f'PASS: {len(objects)} objects in {archive} ({len(REQUIRED)} entry points, {len(undefined)} kernel imports{checked})')
    print(f'      link it with CONFIG_XNU_EXTRA_LD_ARCHIVES="{tree}/build/dby-torch/libDbyTorch.a"')


if __name__ == '__main__':
    main()
