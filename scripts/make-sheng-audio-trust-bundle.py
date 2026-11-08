#!/usr/bin/env python3
"""Build the owner trust bundle for the audio plug-in: one owner module holding the HAL's CDHash, then the original Apple
trust modules byte for byte.

  make-sheng-audio-trust-bundle.py ORIGINAL.bundle TRUST.json OUTPUT.bundle

ORIGINAL.bundle is the unchanged static/System trust bundle that the payload already ships (build/original-static-system-trust,
sha256 7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7); it is verified before anything is written. TRUST.json is
the output of scripts/build-sheng-audio-hal.py and carries the plug-in executable's CDHash. The audio server only loads the plug-in
once that hash is trusted, so the bundle is the payload input: `unixnu build payload --owner-trust-cache OUTPUT.bundle` (it skips the
board.toml [payload] trust_cache_sha256 pin on purpose, because the owner module is exactly what this board adds). Ported from core's
scripts/make-owner-trust-bundle.py, whose owner module carries the GPU plugin's three hashes instead of one.
"""
import hashlib
import json
from pathlib import Path
import struct
import sys
import uuid

if len(sys.argv) != 4:
    sys.exit(__doc__)
original_path, trust_path, out = (Path(arg).resolve() for arg in sys.argv[1:4])
original = original_path.read_bytes()
if hashlib.sha256(original).hexdigest() != '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7':
    sys.exit('The original static/System trust bundle differs from the unchanged target input')
count = struct.unpack_from('<I', original)[0]
assert count == 2
offsets = struct.unpack_from('<2I', original, 4)
originals = [original[offsets[0]:offsets[1]], original[offsets[1]:]]
trust = json.loads(trust_path.read_text())
# DarwinBoot's ValidTrustCaches (and the kernel's binary search) need the hashes of a module in strictly ascending order.
hashes = sorted({bytes.fromhex(trust['cdhash'])})
if len(hashes) != 1:
    sys.exit('Duplicate CDHash')
identifier = uuid.uuid5(uuid.NAMESPACE_URL, 'org.unixnu.sheng.audio:' + ''.join(h.hex() for h in hashes))
own = struct.pack('<I16sI', 2, identifier.bytes, len(hashes)) + b''.join(h + bytes([2, 0, 0, 0]) for h in hashes)
modules = [own, *originals]
cursor, starts = 16, []
for module in modules:
    starts.append(cursor)
    cursor += len(module)
bundle = struct.pack('<4I', 3, *starts) + b''.join(modules)


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


if not valid_like_the_loader(bundle):
    sys.exit('The trust bundle would be refused by the boot loader')
if out.exists():
    sys.exit('Refuse to overwrite ' + str(out))
out.write_bytes(bundle)
out.with_suffix('.json').write_text(json.dumps(dict(
    bundle_sha256=hashlib.sha256(bundle).hexdigest(), own_cdhashes=[h.hex() for h in hashes], original_modules_unchanged=True,
    apple_trust_chain_verified=False, purpose='owner trust for the board audio server plug-in only'), indent=2) + '\n')
print('PASS: owner trust bundle (own module + both original modules)', hashlib.sha256(bundle).hexdigest())
