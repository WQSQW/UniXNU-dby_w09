#!/usr/bin/env python3
"""Stage the owner-built audio server plug-in into a copy of a packed Data seed.

  add-sheng-audio-data.py BASE.dbr OUTPUT.dbr

Adds /private/var/ngpu/audio/ShengAudio.driver (from build/sheng-audio-hal, see scripts/build-sheng-audio-hal.py). The kernel's
boot-time addon mount (sheng-gpu-addon=1, patches/xnu/series-audio) presents that directory as a read-only union view over
/System/Library/Audio/Plug-Ins/HAL, where the audio server loads every driver bundle; without it there is no IOAudio2 device,
so the volume slider has nothing to talk to. This is the route the official GPU plugin uses (docs/zh/drivers/adreno740.md:36,
docs/zh/porting/gpu.md:95-99): the staged seed is the file passed to `unixnu build payload --data-ramdisk`, so the plug-in
travels inside the payload and no boot image and no loader build is involved. The alternative official carrier, the addon blob
(scripts/make-gpu-addon.py, Payloads/kernel.macho.gpuaddon), needs a loader that publishes it to the kernel, which is why it is
not used here. BASE is not modified and the volume UUID is unchanged, so no kernel rebuild follows. Nothing is written to Git
or to the OS inputs.

The device repository carries no DarwinBoot submodule, so the unpacker and packer are core's: point UNIXNU_CORE at the core
checkout (it is the tree that ships drivers/DarwinBoot/Test/packed_ramdisk.py).
"""
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = Path(os.environ.get('UNIXNU_CORE', str(ROOT))).resolve()
TEST = core / 'drivers/DarwinBoot/Test'
if not (TEST / 'packed_ramdisk.py').is_file():
    sys.exit(f'No {TEST}/packed_ramdisk.py: set UNIXNU_CORE to the UniXNU core checkout')
sys.path.insert(0, str(TEST))
import packed_ramdisk

BUNDLE = ROOT / 'build/sheng-audio-hal/ShengAudio.driver'
EXECUTABLE = 'ShengAudio'


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    base, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    if output.exists():
        sys.exit('Refuse to overwrite ' + str(output))
    if not (BUNDLE / EXECUTABLE).is_file():
        sys.exit('Build the plug-in first: python3 scripts/build-sheng-audio-hal.py build/sheng-audio-hal')
    blob = base.read_bytes()
    logical, extents = packed_ramdisk.regions(blob)
    with tempfile.TemporaryDirectory(prefix='data-seed-', dir=str(ROOT / 'build')) as td:
        raw = Path(td) / 'seed.raw'
        with raw.open('wb') as f:
            f.truncate(logical)
            for offset, data in extents:
                f.seek(offset)
                f.write(data)
        mount = Path(td) / 'mount'
        mount.mkdir()
        attached = plistlib.loads(subprocess.check_output(
            ['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse', '-owners', 'off',
             '-mountpoint', str(mount), '-plist', str(raw)]))
        device = attached['system-entities'][0]['dev-entry']
        try:
            if not (mount / 'mobile').is_dir():
                sys.exit('The Data seed has no /mobile; wrong base image?')
            target = mount / 'ngpu/audio/ShengAudio.driver'
            if target.exists():
                shutil.rmtree(target)
            target.parent.mkdir(parents=True)
            for directory in (mount / 'ngpu', mount / 'ngpu/audio'):
                os.chmod(directory, 0o755)
            shutil.copytree(BUNDLE, target, symlinks=False)
            for path in [target, *target.rglob('*')]:
                os.chmod(path, 0o755 if path.is_dir() or path.name == EXECUTABLE else 0o644)
            print('staged', target.relative_to(mount), sorted(p.name for p in target.iterdir()))
        finally:
            subprocess.run(['hdiutil', 'detach', device], check=True)
        # The four magic bytes, read at their offset: core reads them as raw.read_bytes()[32:36], which materialises the whole
        # expanded container and got the runner killed (exit 137) on the grown seed. Same assertion, no allocation.
        with raw.open('rb') as probe:
            probe.seek(32)
            magic = probe.read(4)
        if magic != b'NXSB':
            sys.exit('The edited image is no APFS container any more')
        metadata = packed_ramdisk.pack(raw, output)
    report = dict(base_sha256=packed_ramdisk.inspect(base)[0]['sha256'], output=metadata,
                  plugin=json.loads((BUNDLE.parent / 'trust.json').read_text()))
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
