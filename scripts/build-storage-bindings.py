#!/usr/bin/env python3
"""Bind a source block driver to the original IOStorageFamily symbols of the pinned kernel.

The addresses come from one of two sources, in this order:

1. build/storage-abi/interface-audit.json, the recorded result of
   scripts/audit-storage-interface.py. That audit compiles the public reference class,
   finds the original dispatch table in the kernelcache and derives every address, then
   additionally asserts it against a live GDB stop.
2. scripts/data/native-storage-original-bindings.S, the recorded static derivation. It is
   used when the audit result is absent, so a build tree without the qualification
   evidence still links. The comment block of that file records the derivation steps.

Either way this recipe writes an assembly file with absolute symbol addresses, assembles
it and puts it into the driver archive, because IOKit is prelinked and exports none of
these symbols.

The kernel link re-reads the recorded object through the archive stem
(core/scripts/build-xnu-4k-baseline.sh:709) while the component recipe names the object
after the runtime class, so this recipe also publishes those bytes under the archive stem.
The gate compares sha256, so an alias is the same compiled object, never a second build.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
import native_prelink as np

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--component', default='NativeUfs', choices=['NativeUfs', 'NativeVirtioBlock', 'NativeRamBlock'])
# The archive recipe of the component names its output; the runtime class name and that
# archive name differ (NativeUfs is built into libShengUfs.a), so the archive is explicit.
parser.add_argument('--archive', required=True, help='component archive to add the bindings object to')
parser.add_argument('--directory', type=Path, default=ROOT / 'build/sheng-ufs')
# The recorded file is one union of original storage symbols. A driver that imports only part
# of it is still fully covered, so an unused entry is not an error there. The default stays
# strict: for the component the file was derived from, an unused entry means the file is stale.
parser.add_argument('--subset', action='store_true',
                    help='bind only the symbols this object imports; unused recorded entries are allowed')
args = parser.parse_args()

out = args.directory.resolve()
component = args.component
archive = out / args.archive
obj_path = out / (component + '.o')
if not obj_path.is_file():
    raise SystemExit(f'{obj_path} not found; build the component first')

audit_path = ROOT / 'build/storage-abi/interface-audit.json'
recorded = ROOT / 'scripts/data/native-storage-original-bindings.S'


def object_symbols(path):
    data = path.read_bytes()
    image = np.macho(data)
    offset = next(at for command, at, size in image['commands'] if command == 2)
    symoff, count, stroff, _ = struct.unpack_from('<4I', data, offset + 8)
    symbols = []
    for index in range(count):
        strx, kind, _, _, value = struct.unpack_from('<IBBHQ', data, symoff + index * 16)
        symbols.append((data[stroff + strx:data.index(b'\0', stroff + strx)].decode(), value, kind))
    return data, image, symbols


def undefined_of(symbols):
    return sorted({name for name, value, kind in symbols if kind == 1 and
                   ('IOBlockStorageDevice' in name or 'IOMedia' in name or 'IOStorage' in name)})


data, image, symbols = object_symbols(obj_path)
imports = undefined_of(symbols)


def assemble(source: Path, weak=False):
    # Weak mode is written as `.weak` in the source itself, so nothing else in the object changes.
    subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror',
                    '-Wno-incompatible-sysroot', '-c', source, '-o', out / 'original-bindings.o'],
                   check=True)


def binding_source(bindings, weak=False):
    # Mach-O spells a weak definition .weak_definition; there is no .weak directive.
    directive = '.weak_definition' if weak else '.globl'
    return ''.join(f'{directive} {name}\n.set {name}, {value:#x}\n' for name, value in sorted(bindings.items()))


def archive_with(bindings, provenance, weak=False):
    assemble(out / 'original-bindings.S', weak)
    # The component recipe already archived its own object; this adds the bindings object
    # to that same archive, which is the file the kernel link force-loads.
    subprocess.run(['xcrun', 'ar', 'rcs', archive, obj_path, out / 'original-bindings.o'], check=True)
    alias = archive.with_name(archive.stem.removeprefix('lib') + '.o')
    if alias != obj_path:
        alias.unlink(missing_ok=True)
        try:
            os.link(obj_path, alias)
        except OSError:
            shutil.copyfile(obj_path, alias)
    (out / 'bindings.json').write_text(json.dumps(dict(
        component=component, archive=archive.name, provenance=provenance,
        compiled_object_sha256=hashlib.sha256(data).hexdigest(),
        bindings={name: hex(value) for name, value in sorted(bindings.items())},
        original_code_modified=False, original_images_modified=False,
        actual_io_qualified=False), indent=2) + '\n')
    print(f'PASS: {component} bound to {len(bindings)} original IOStorageFamily symbols ({provenance})')


if audit_path.is_file():
    audit = json.loads(audit_path.read_text())
    if not audit.get('live_native_4k') or audit.get('method_slots') != 190:
        raise SystemExit('The storage audit does not describe the qualified original ABI')
    possible = {row['reference_name']: int(row['candidate_target'], 16)
                for row in audit['methods'] if not row['kernel_names']}
    possible.update({
        '__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass': int(audit['original_protected_constructor'], 16),
        '__ZN20IOBlockStorageDeviceD2Ev': int(audit['original_base_destructor'], 16),
        '__ZN20IOBlockStorageDevice10gMetaClassE': int(audit['original_metaclass'], 16),
        '__ZTV20IOBlockStorageDevice': int(audit['vtable'], 16)})
    bindings = {name: possible[name] for name in imports if name in possible}
    missing = set(imports) - set(bindings)
    if missing:
        raise SystemExit('The storage audit does not cover: ' + ', '.join(sorted(missing)))
    (out / 'original-bindings.S').write_text(binding_source(bindings, weak=args.subset))
    archive_with(bindings, 'build/storage-abi/interface-audit.json')
else:
    if not recorded.is_file():
        raise SystemExit('Neither the storage audit nor the recorded bindings are present')
    recorded_values = {}
    for line in recorded.read_text().splitlines():
        match = re.match(r'\.set (\S+), (0x[0-9a-f]+)$', line.strip())
        if match:
            recorded_values[match.group(1)] = int(match.group(2), 16)
    missing = set(imports) - set(recorded_values)
    if missing:
        raise SystemExit('The recorded bindings do not cover: ' + ', '.join(sorted(missing)))
    extra = set(recorded_values) - set(imports)
    if extra and not args.subset:
        raise SystemExit('The recorded bindings carry unused symbols: ' + ', '.join(sorted(extra)))
    # A subset import is the second driver to bind the same original addresses (VirtioMmioBlock
    # next to ShengUfs), so its definitions are weak: when both archives are force-loaded, the
    # first one supplies the symbol and the other agrees with it instead of colliding.
    bindings = {name: recorded_values[name] for name in imports} if args.subset else dict(recorded_values)
    (out / 'original-bindings.S').write_text(binding_source(bindings, weak=args.subset))
    archive_with(bindings, 'scripts/data/native-storage-original-bindings.S')

