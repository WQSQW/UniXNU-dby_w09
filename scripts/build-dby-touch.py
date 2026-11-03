#!/usr/bin/env python3
"""Build the DBY-W09 touch archive against this source kernel's actual ABI.

`libDbyTouch.a` is linked in through `CONFIG_XNU_EXTRA_LD_ARCHIVES` (plain archive path, not
force_load: package/BootProgress.c holds a strong reference to `dby_touch_probe_entry()`, so the
lazy pull always happens). The board hook in BootProgress.c calls that entry once the first user
process exists, and the driver stays inert unless the payload's kernel arguments carry
`dby-touch=<stage>`.

The implementation is the one proven on hardware on an identical DBY-W09 (same board, same
Novatek N9Z no-flash THP panel) and adopted verbatim: frame contract 8208 bytes / 50x32 offset
binary, per-node minimum reference, 4-connected blob extraction, publish through the
`native_touch_*` seqlock the unchanged original HID layer consumes, and the vendor firmware
upload the closed HAL performs at every start (this part has no flash, so a host that never
uploads never reports).

Both translation units are compiled with commands the kernel build itself recorded, so headers,
defines and ABI flags are the ones this kernel was really built with:

  * DbyTouchProtocol.c  <- build/xnu/ios-obj/.../osfmk/DEVELOPMENT/startup.o.json     (C)
  * DbyTouch.cpp        <- build/xnu/ios-obj/.../iokit/DEVELOPMENT/IOPlatformExpert.cpo.json (C++)
  * GeniSpi.c           <- osfmk C record, compiled from the official QcomGeniSpi component

The recorded commands carry no -Werror; this script puts -Werror at the front and then restores
the warning demotions the driver's own build log carries at the end, because xnu's OSMetaClass
and KALLOC_TYPE_DEFINE macros trip -Wmissing-designated-field-initializers / -Wc99-designator /
-Wreorder-init-list / -Wunnecessary-virtual-specifier on every driver that uses them.

Source lookup order is tree/drivers/DbyTouch, tree/kits/dby_w09/DbyTouch, package/DbyTouch --
kits/dby_w09 is this repository's package/, so the in-repo sources are found with no copy step.
GeniSpi.c is deliberately NOT copied into this package (the component's AGENTS.md forbids a second
copy, which would drift from upstream); it is compiled where the tree links it, and it stays C,
because GeniSpi.h has no extern "C" and a C++ build would change the names it exports.

Usage:
  python3 scripts/build-dby-touch.py
  python3 scripts/build-dby-touch.py --tree <assembled tree> --werror
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARCH = 'DEVELOPMENT_ARM64_BCM2837'

# name -> (compile record, path relative to the tree; None means "in the package directory")
RECORDS = {
    'DbyTouchProtocol.c': ('osfmk/DEVELOPMENT/startup.o.json', None),
    'DbyTouch.cpp': ('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json', None),
    'GeniSpi.c': ('osfmk/DEVELOPMENT/startup.o.json', 'drivers/QcomGeniSpi/GeniSpi.c'),
}

DEMOTIONS = [
    '-Wno-missing-designated-field-initializers',
    '-Wno-c99-designator',
    '-Wno-reorder-init-list',
    '-Wno-unnecessary-virtual-specifier',
]

# -Weverything is what the kernel's own compile uses, but the kernel never turns warnings into
# errors; combined with -Werror it drags in diagnostics the kernel does not demote and that a
# driver cannot satisfy without rewriting kernel-owned macros (clang's -Wunsafe-buffer-usage
# fires on every array access in DbyTouchProtocol.c). -Wall -Wextra -Werror is kept, the umbrella
# is dropped, and the report records that so nobody believes the archive was built under it.
DROPPED = {'-Weverything'}


def build_command(record_path, source, obj, include_dirs):
    """Reuse the kernel's own recorded command, with -Werror in front and the demotions last."""
    record = json.loads(Path(record_path).read_text().strip().rstrip(','))
    args = list(record['arguments'])
    command = [args[0], '-Werror']
    i = 1
    skipped_includes = []
    while i < len(args):
        arg = args[i]
        if arg in DROPPED:
            i += 1
            continue
        if arg == record['file']:
            command.append(str(source))
        elif arg == '-I' and i + 1 < len(args):
            # A pruned tree no longer carries every include directory the kernel had (EXPORT_HDRS
            # /libsa is the usual one) and -Werror turns -Wmissing-include-dirs into a failure.
            directory_arg = args[i + 1]
            if Path(directory_arg).is_dir():
                command += [arg, directory_arg]
            else:
                skipped_includes.append(directory_arg)
            i += 1
        elif arg in ('-o', '-MF', '-MT', '-MJ'):
            if arg == '-o':
                command += ['-o', str(obj)]
            i += 1
        else:
            command.append(arg)
        i += 1
    command += ['-Wall', '-Wextra', *DEMOTIONS]
    for directory in include_dirs:
        command += ['-I', str(directory)]
    if '-o' not in command:
        command += ['-o', str(obj)]
    return command, record['directory'], skipped_includes


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path,
                        default=Path(os.environ['UNIXNU_TREE']) if os.environ.get('UNIXNU_TREE') else None,
                        help='assembled kernel tree; defaults to this script\'s own tree root '
                             '(that is what `unixnu` uses when it runs an archive recipe) or '
                             '$UNIXNU_TREE')
    parser.add_argument('--source-dir', type=Path, help='DbyTouch sources; defaults to the lookup order')
    parser.add_argument('--werror', action='store_true',
                        help='kept for the documented invocation; -Werror is always on')
    args = parser.parse_args()

    # `unixnu` copies scripts/ into <tree>/scripts and runs the recipe there, so the script's own
    # parent-of-parent is the tree in the normal build path. --tree/$UNIXNU_TREE overrides it for
    # the manual "workspace repo against a tree somewhere else" case.
    tree = (args.tree or ROOT).resolve()
    if not (tree / 'build/xnu/ios-obj' / ARCH).is_dir():
        raise SystemExit(f'{tree} has no build/xnu/ios-obj/{ARCH}: point --tree at the assembled tree')
    # The archive must land where CONFIG_XNU_EXTRA_LD_ARCHIVES looks for it.
    out = tree / 'build/dby-touch'

    candidates = [args.source_dir] if args.source_dir else [
        tree / 'drivers/DbyTouch',
        tree / 'kits/dby_w09/DbyTouch',
        ROOT / 'package/DbyTouch',
    ]
    source_dir = next((c.resolve() for c in candidates if c and (c / 'DbyTouch.cpp').is_file()), None)
    if source_dir is None:
        raise SystemExit('DbyTouch.cpp not found in: ' + ', '.join(str(c) for c in candidates))
    geni_dir = tree / 'drivers/QcomGeniSpi'
    if not geni_dir.is_dir():
        raise SystemExit(f'{geni_dir} missing: the official QcomGeniSpi component must be selected')

    out.mkdir(parents=True, exist_ok=True)
    objects = []
    skipped_all = []
    built = {}
    for name, (record_rel, explicit) in RECORDS.items():
        source = (tree / explicit) if explicit else (source_dir / name)
        if not source.is_file():
            raise SystemExit(f'{source} missing')
        built[name] = source
        record_path = tree / 'build/xnu/ios-obj' / ARCH / record_rel
        if not record_path.is_file():
            raise SystemExit(f'{record_path} missing: build the kernel once before this archive')
        obj = out / (source.stem + '.o')
        # Every translation unit may include the upstream QUPv3 GENI/SPI header, so its directory
        # is on the include path for all of them, not just for GeniSpi.c itself.
        include_dirs = [source_dir, geni_dir]
        if source.parent != source_dir:
            include_dirs.insert(1, source.parent)
        command, directory, skipped = build_command(record_path, source, obj, include_dirs)
        if skipped:
            print('note: skipped missing include dirs: ' + ', '.join(skipped))
        (out / f'compile-command-{source.stem}.json').write_text(json.dumps(command, indent=2) + '\n')
        subprocess.run(command, cwd=directory, check=True, timeout=600)
        objects.append(obj)
        skipped_all += skipped

    archive = out / 'libDbyTouch.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *[str(o) for o in objects]],
                   check=True, timeout=60)

    report = {
        'tree': str(tree),
        'source_dir': str(source_dir),
        'sources': {name: hashlib.sha256(path.read_bytes()).hexdigest()
                    for name, path in built.items()},
        'objects': {o.name: o.stat().st_size for o in objects},
        'archive': archive.name,
        'archive_bytes': archive.stat().st_size,
        'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
        'dropped_warning_umbrella': sorted(DROPPED),
        'skipped_include_dirs': skipped_all,
        'hardware_execution': False,
    }
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: libDbyTouch.a built from ' + str(source_dir))
    print(f'      archive {archive} ({report["archive_bytes"]} bytes)')
    print('      link it with CONFIG_XNU_EXTRA_LD_ARCHIVES="{tree}/build/dby-touch/libDbyTouch.a"')


if __name__ == '__main__':
    main()
