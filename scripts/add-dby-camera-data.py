#!/usr/bin/env python3
"""Stage the owner-built MediaCapture plug-in into a copy of a packed Data seed.

  add-dby-camera-data.py [--plugin DIR] [--test-pattern] BASE.dbr OUTPUT.dbr

Adds /private/var/ngpu/mediacapture/H6ISP.mediacapture (DIR defaults to build/dby-camera-plugin, the output of
scripts/build-dby-camera-plugin.py). The kernel's boot-time addon mount (dby-torch=1 dby-ti=1,
patches/xnu/zze-dby-torch-media-mount.patch, the fifth mount) presents that directory as a read-only union view over
/System/Library/MediaCapture, which is the path BWFigCaptureDeviceVendor dlopens for a class named AppleH6CamIn.
Without that file the vendor has no capture device and +[AVFlashlight hasFlashlight] has nothing to open, which is why
the Control Center toggle does not appear even though the kernel publishes the class (package/DbyTorch/DbyTorchIos.h).
This is the same carrier the audio server plug-in uses (scripts/add-sheng-audio-data.py) and the route the official GPU
plugin documents (docs/zh/porting/gpu.md:95-99): the staged seed is the file passed to `unixnu build payload
--data-ramdisk`, so the plug-in travels inside the payload, and staging keeps the volume UUID, so it costs no kernel
rebuild. The signature is ad hoc, so the CDHash in DIR/trust.json also has to enter the owner trust cache - see
scripts/add-dby-camera-trust.py. --test-pattern additionally places the empty marker file ios/DbyCameraProvider.h
calls DBC_MARKER, which is what switches the camera provider on; without it the plug-in carries the vendor-class fix
and the flashlight only. BASE is not modified and nothing is written to Git or to the OS inputs. Runs on macOS
(hdiutil, codesign).

The device repository carries no DarwinBoot submodule, so the unpacker and packer are core's: point UNIXNU_CORE at the core
checkout (it is the tree that ships drivers/DarwinBoot/Test/packed_ramdisk.py).
"""
import argparse
import hashlib
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

SEED_DIR = 'ngpu/mediacapture'
NAME = 'H6ISP.mediacapture'
MARKER = 'DbyCameraTestPattern'
BUNDLE = ROOT / 'build/dby-camera-plugin'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--plugin', type=Path, default=BUNDLE)
    parser.add_argument('--test-pattern', action='store_true', help='also place the marker that switches the camera provider on')
    parser.add_argument('base')
    parser.add_argument('output')
    args = parser.parse_args()
    directory, base, output = args.plugin.resolve(), Path(args.base).resolve(), Path(args.output).resolve()
    if output.exists():
        sys.exit('Refuse to overwrite ' + str(output))
    plug_in, trust_file = directory / NAME, directory / 'trust.json'
    if not plug_in.is_file() or not trust_file.is_file():
        sys.exit(f'Build the plug-in first: python3 scripts/build-dby-camera-plugin.py --out {directory}')
    trust = json.loads(trust_file.read_text())
    if trust.get('identifier') != 'org.unixnu.dby_w09.camera':
        sys.exit(f'{plug_in} is not the plug-in of scripts/build-dby-camera-plugin.py')
    subprocess.run(['codesign', '--verify', '--strict', str(plug_in)], check=True)
    blob = base.read_bytes()
    logical, extents = packed_ramdisk.regions(blob)
    with tempfile.TemporaryDirectory(prefix='camera-seed-', dir=str(output.parent)) as td:
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
            (mount / SEED_DIR).mkdir(parents=True, exist_ok=True)
            for parent in (mount / 'ngpu', mount / SEED_DIR):
                os.chmod(parent, 0o755)
            target = mount / SEED_DIR / NAME
            replaced = None
            if target.exists():
                replaced = hashlib.sha256(target.read_bytes()).hexdigest()
                target.unlink()
            shutil.copy2(plug_in, target)
            os.chmod(target, 0o755)
            subprocess.run(['codesign', '--verify', '--strict', str(target)], check=True)
            marker = mount / SEED_DIR / MARKER
            marker.unlink(missing_ok=True)
            if args.test_pattern:
                marker.write_bytes(b'')
                os.chmod(marker, 0o644)
            print('staged', target.relative_to(mount), 'marker', bool(args.test_pattern), 'replaced', replaced)
            usage = subprocess.run(['df', '-k', str(mount)], capture_output=True, text=True).stdout.splitlines()[-1].split()
            used_kib, available_kib = int(usage[2]), int(usage[3])
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
                  plugin=dict(path='/private/var/' + SEED_DIR + '/' + NAME, **trust),
                  test_pattern=args.test_pattern, replaced_sha256=replaced,
                  used_kib=used_kib, available_kib=available_kib)
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
