#!/usr/bin/env python3
"""Add the MediaCapture plug-in's CDHash to a copy of the owner trust bundle.

  add-dby-camera-trust.py BASE.bundle TRUST.json OUTPUT.bundle [--original BUNDLE] [--drop-cdhash HEX ...]

BASE.bundle is the owner trust bundle the payload already uses (scripts/make-sheng-audio-trust-bundle.py makes the first one,
and it holds the audio server plug-in's CDHash); TRUST.json is the output of scripts/build-dby-camera-plugin.py, so the hash
comes from the exact bytes that were compiled. The plug-in is signed ad hoc, so mediaserverd - a platform process - refuses to
map its code until its CDHash is in a trusted cache, which is why the seed and this bundle have to advance together.
The bundle keeps ONE owner module (make-owner-trust-bundle.py's shape): the new hash joins the existing ones there in ascending
order, because DarwinBoot's ValidTrustCaches and the kernel's binary search both require sorted hashes, and the module identifier
is derived from the sorted list. The owner module is found by that derivation, not by its position: its identifier is
uuid5(NAMESPACE_URL, namespace + its hashes), which is how both producers of this board's bundles name it
(trust-append-module.py uses org.unixnu.owner: and puts the module last, make-sheng-audio-trust-bundle.py uses
org.unixnu.sheng.audio: and puts it first), and the new bundle keeps the namespace it found. --original additionally compares the
remaining modules with the unchanged static/System trust bundle and refuses a difference. --drop-cdhash removes a hash that no
longer names anything the board ships (a rebuilt plug-in replaces its predecessor); a hash that is not in the bundle is refused,
so a typo cannot pass silently. BASE is not modified. Writes OUTPUT and OUTPUT with .json.

This is PaganiAudio's add_trust() (refer/PaganiDrivers/drivers/PaganiAudio/scripts/add-pagani-audio-data.py), which
add-pagani-camera-data.py calls through the torch script, as its own file: the mother's three scripts import each other and this
repository keeps each step in one script. Pure struct work, so unlike the seed staging it runs on the Linux host.
"""
import argparse
import hashlib
import json
import re
import struct
import sys
import uuid
from pathlib import Path

# The namespaces this repository's two trust-bundle builders derive the owner module's identifier from.
OWNER_NAMESPACES = ('org.unixnu.owner:', 'org.unixnu.sheng.audio:')
ORIGINAL_SHA256 = '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7'


def fail(message):
    raise SystemExit('add-dby-camera-trust: ' + message)


def modules_of(data):
    count = struct.unpack_from('<I', data)[0]
    if not 0 < count <= 128 or len(data) < 4 + 4 * count:
        fail('not a trust bundle')
    starts = list(struct.unpack_from(f'<{count}I', data, 4))
    ends = starts[1:] + [len(data)]
    return [data[start:end] for start, end in zip(starts, ends)]


def bundle_of(modules):
    cursor, starts = 4 + 4 * len(modules), []
    for module in modules:
        starts.append(cursor)
        cursor += len(module)
    return struct.pack(f'<{1 + len(modules)}I', len(modules), *starts) + b''.join(modules)


def valid_like_the_loader(data):
    # The same structural checks as drivers/DarwinBoot/Test ... ValidTrustCaches in core's builder.
    count = struct.unpack_from('<I', data)[0]
    if not 0 < count <= 128:
        return False
    for index in range(count):
        start = struct.unpack_from('<I', data, 4 + 4 * index)[0]
        end = struct.unpack_from('<I', data, 8 + 4 * index)[0] if index + 1 < count else len(data)
        if start < 4 + 4 * count or start & 3 or end > len(data) or end - start < 24 or struct.unpack_from('<I', data, start)[0] != 2:
            return False
        entries = struct.unpack_from('<I', data, start + 20)[0]
        if end - start != 24 + entries * 24:
            return False
        for entry in range(entries):
            at = start + 24 + entry * 24
            if not 1 <= data[at + 20] <= 4 or (entry and data[at - 24:at - 4] >= data[at:at + 20]):
                return False
    return True


def entries_of(module):
    count = struct.unpack_from('<I', module, 20)[0]
    return [module[24 + 24 * i:48 + 24 * i] for i in range(count)]


def owner_of(modules):
    """The owner module by its own derivation: identifier == uuid5(NAMESPACE_URL, namespace + its hashes)."""
    for index, module in enumerate(modules):
        hashes = entries_of(module)
        joined = ''.join(h[:20].hex() for h in hashes)
        for namespace in OWNER_NAMESPACES:
            if uuid.UUID(bytes=module[4:20]) == uuid.uuid5(uuid.NAMESPACE_URL, namespace + joined):
                return index, namespace
    return None, None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--original')
    parser.add_argument('--drop-cdhash', action='append', default=[], metavar='HEX')
    parser.add_argument('base')
    parser.add_argument('trust')
    parser.add_argument('output')
    args = parser.parse_args()
    base, trust_file, out = (Path(arg).resolve() for arg in (args.base, args.trust, args.output))
    original_path = Path(args.original).resolve() if args.original else None
    if out.exists():
        fail('Refuse to overwrite ' + str(out))
    cdhash = json.loads(trust_file.read_text()).get('cdhash')
    if not isinstance(cdhash, str) or not re.fullmatch(r'[0-9a-f]{40}', cdhash):
        fail(f'{trust_file} holds no 40-digit lower-case hex CDHash; rebuild the plug-in')
    data = base.read_bytes()
    if not valid_like_the_loader(data):
        fail(f'{base} is not a trust bundle the boot loader accepts')
    modules = modules_of(data)
    owner, namespace = owner_of(modules)
    if owner is None:
        fail(f'{base} has no owner module: none of its {len(modules)} identifiers is the uuid5 of its own hashes'
             f' under {", ".join(OWNER_NAMESPACES)}')
    apple = []
    if original_path:
        blob = original_path.read_bytes()
        if hashlib.sha256(blob).hexdigest() != ORIGINAL_SHA256:
            fail(f'{original_path} is not the unchanged original static/System trust bundle')
        apple = modules_of(blob)
        if [m for i, m in enumerate(modules) if i != owner] != apple:
            fail(f'{base} does not carry the original Apple modules unchanged beside its owner module')
    hashes = {entry[:20]: entry[20:] for entry in entries_of(modules[owner])}
    added = bytes.fromhex(cdhash)
    dropped = []
    for value in args.drop_cdhash:
        if not re.fullmatch(r'[0-9a-f]{40}', value):
            fail(f'--drop-cdhash {value}: not a 40-digit lower-case hex CDHash')
        old = bytes.fromhex(value)
        if old == added:
            continue
        if old not in hashes:
            fail(f'{base} has no owner CDHash {value} to drop')
        del hashes[old]
        dropped.append(value)
    already = added in hashes
    hashes.setdefault(added, bytes([2, 0, 0, 0]))   # hash type 2 (SHA-256, truncated), no flags, no constraint category
    ordered = sorted(hashes)
    identifier = uuid.uuid5(uuid.NAMESPACE_URL, namespace + ''.join(h.hex() for h in ordered))
    own = struct.pack('<I16sI', 2, identifier.bytes, len(ordered)) + b''.join(h + hashes[h] for h in ordered)
    modules[owner] = own
    result = bundle_of(modules)
    if not valid_like_the_loader(result):
        fail('the new trust bundle would be refused by the boot loader')
    out.write_bytes(result)
    report = dict(base=str(base), base_sha256=hashlib.sha256(data).hexdigest(), already_present=already, dropped=dropped,
                  added=cdhash, owner_module=owner, owner_namespace=namespace, owner_hashes=[h.hex() for h in ordered],
                  owner_uuid=str(identifier), apple_modules_unchanged=bool(apple),
                  bundle_sha256=hashlib.sha256(result).hexdigest(), modules=len(modules))
    out.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
