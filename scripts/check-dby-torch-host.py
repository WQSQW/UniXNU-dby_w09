#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""DbyTorch host checks: the portable sources compiled strictly, then each test with ASan/UBSan and -Werror.

  spmi   package/DbyTorch/DbyTorchSpmi.c against a model of the SM8250 v5 arbiter and the PM8150L flash block (discovery over the whole APID
         table, the owned-mapping preference, the command-word shapes this board really takes, the ladder's memory and its walking, the write
         whitelist over every one of the 256 bytes, the interlock, failures that keep their reasons apart, the one emergency stop)
  core   package/DbyTorch/DbyTorchCore.c on the same model (the boot arguments' configuration, the current law with the PM8150L resolution
         erratum, the dry run, refusals before any write, the exact on and off sequences of leds-qpnp-flash-v2.c, every level of the torch
         node's 300 mA, the 5 minute limit, a thermal derate that only comes down, every fault bit, a dead ladder at every one of the run's
         12 writes and 48 reads, one transient error at every one of them, a register that will not take the byte, the log's ration)
  ios    package/DbyTorch/DbyTorchIos.c: the plan the two torch boot arguments make, the last check before launchd (no service without
         both the driver and the plug-in), the level a user client call may carry, and the names the other side of the chain looks for
  args   every boot argument the glue reads is in the component README's table and in DbyTorch.h, and they name no other
  glue   with --tree (or $UNIXNU_TREE, or run from the assembled tree): the kernel archive, built with that tree's own XNU compile records
         by scripts/build-dby-torch.py into a temporary directory, so nothing in the tree is written (the entry points, the kernel-import
         allowlist, every import against that tree's own linked kernel when it has one, the class initializer staged out of
         __mod_init_func and nothing else, Mach-O iOS 17)
  mount  with --tree: scripts/test-dby-torch-media-mount-patch.py --inputs <the tree's own xnu-inputs.json>, so
         patches/xnu/zze-dby-torch-media-mount.patch is replayed after the whole patch series that kernel build
         recorded, in that order, onto a fresh clone of the same commit -- the same thing scripts/unixnu/patches.py
         prepare() does, which is the only way to see a lost context or a second claim on one scratch window

  check-dby-torch-host.py [--tree TREE]   (TREE: also the kernel archive; default $UNIXNU_TREE, or this script's own tree)

The glue half runs in the kernel step's environment: `xcrun ar` has to reach the Mach-O archiver, and scripts/linux's xcrun shim calls that
directory $XNU_TOOLCHAIN_BIN. Do not put it first in PATH instead -- its `as` is Apple's, and the host half is GCC against glibc. The objcopy
shim of LLVM 20 does belong first in PATH, because staging the class initializer is Mach-O section surgery.

The glue is C++ against XNU's own headers, so it needs a tree: the records a real kernel build leaves are what say what the headers, the ABI
flags and the warnings of this kernel are. That is how scripts/build-dby-touch.py compiles this board's touch driver, and there is no
stand-in for it here. Host checks do not establish that the LED lights on the device."""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/dby_w09/DbyTorch' if (ROOT / 'kits/dby_w09/DbyTorch').is_dir() else ROOT / 'package/DbyTorch'
ARCH = 'DEVELOPMENT_ARM64_BCM2837'
CORE = ['DbyTorchSpmi.c', 'DbyTorchCore.c', 'DbyTorchIos.c']
GLUE = 'DbyTorch.cpp'
TESTS = {
    'spmi': ['Test/spmi.c', 'DbyTorchSpmi.c'],
    'core': ['Test/core.c', 'DbyTorchCore.c', 'DbyTorchSpmi.c'],
    'ios': ['Test/ios.c', 'DbyTorchIos.c'],
}
STRICT = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wshadow', '-Wconversion', '-Wno-sign-conversion', '-Wcast-qual',
          '-Wstrict-prototypes', '-Wmissing-prototypes', '-O2']
SANITIZE = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        sys.stdout.write(result.stdout + result.stderr)
        raise SystemExit(f'FAIL: {" ".join(map(str, command))}')
    return result.stdout


def check_arguments():
    """Every boot argument the glue reads is in the component README's table and in DbyTorch.h, and they name no
    other: an argument whose name exists in only one of the three is a name nobody can type."""
    read = set(re.findall(r'argument\("(dby-torch[a-z0-9-]*)"', (PACKAGE / GLUE).read_text()))
    if 'dby-torch' not in read or len(read) < 4:
        raise SystemExit(f'FAIL args: the boot argument reads were not found in {GLUE} ({sorted(read)})')
    readme = (PACKAGE / 'README.md').read_text().split('## Boot arguments', 1)[-1].split('\n## ', 1)[0]
    header = (PACKAGE / 'DbyTorch.h').read_text().split('#ifndef', 1)[0]
    problems = []
    for where, text in (('README.md', readme), ('DbyTorch.h', header)):
        named = set(re.findall(r'(?<![a-z-])(dby-torch(?:-[a-z0-9]+)*)', text))
        problems += [f'{name} is read but not in {where}' for name in sorted(read - named)]
        problems += [f'{where} names {name}, which is not read' for name in sorted(named - read)]
    if problems:
        raise SystemExit('FAIL args:\n  ' + '\n  '.join(problems))
    print(f'PASS args: {len(read)} boot arguments read, each in README.md and DbyTorch.h')


def check_mount_patch(tree):
    """Replay the recorded XNU patch series plus the flashlight mount onto a fresh clone, the way the kernel
    build's patch step does it. A mount patch that only applies after its predecessor cannot be judged by
    reading it: its context lines are lines the audio patch inserts."""
    inputs = tree.parent / 'xnu-inputs.json'
    if not inputs.is_file():
        print(f'note: no {inputs}, so the mount patch was not replayed against a recorded patch series')
        return
    result = subprocess.run([sys.executable, '-B', str(ROOT / 'scripts/test-dby-torch-media-mount-patch.py'),
                            '--inputs', str(inputs)], capture_output=True, text=True)
    if result.returncode:
        sys.stdout.write(result.stdout + result.stderr)
        raise SystemExit(f'FAIL: {inputs} + zze-dby-torch-media-mount.patch does not land in order')
    ran = re.search(r'Ran (\d+) tests?', result.stderr)
    print(f'PASS mount patch: {ran.group(1) if ran else "?"} tests against {inputs}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, default=os.environ.get('UNIXNU_TREE'),
                        help="tree whose compile records build the kernel archive (default: $UNIXNU_TREE, or this script's own tree)")
    args = parser.parse_args()
    if not PACKAGE.is_dir():
        raise SystemExit(f'FAIL: {PACKAGE} missing')
    tree = Path(args.tree).resolve() if args.tree else (ROOT if (ROOT / 'build/xnu/ios-obj' / ARCH).is_dir() else None)
    cc = os.environ.get('CC', 'cc')
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for name in CORE:
            run([cc, *STRICT, '-c', str(PACKAGE / name), '-o', str(tmp / (name + '.o'))])
        print(f'PASS strict compile: {len(CORE)} portable sources (-Wconversion -Wshadow -Wmissing-prototypes)')
        for test, sources in TESTS.items():
            exe = tmp / test
            run([cc, *SANITIZE, *(str(PACKAGE / s) for s in sources), '-o', str(exe)])
            out = run([str(exe)]).strip()
            print(out.splitlines()[-1] if out else f'{test}: no output')
        check_arguments()
        if tree:
            print(run([sys.executable, '-B', str(ROOT / 'scripts/build-dby-torch.py'), '--tree', str(tree),
                       '--out', str(tmp / 'kernel')]).strip())
            check_mount_patch(tree)
        else:
            print('note: no compile records here and no --tree/$UNIXNU_TREE, so the kernel archive was not built')
    print('PASS DbyTorch host checks')


if __name__ == '__main__':
    main()
