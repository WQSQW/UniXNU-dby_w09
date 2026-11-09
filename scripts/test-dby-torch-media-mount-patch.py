#!/usr/bin/env python3
"""patches/xnu/zze-dby-torch-media-mount.patch: the fifth native_gpu_addon_mount() in kern_exec.c, the
one that makes the flashlight plug-in visible so dby_torch_ios_commit() will publish AppleH6CamIn and
the /product/camera flash property.

Two things can go wrong with a mount patch and neither is visible in the patch file alone: it can lose
its context because another series edits the same lines (this one is anchored on the two counters and
the call tail that zzyd-sheng-audio-addon-mount.patch adds, so it must run after it), and it can take a
scratch window another mount already takes. The first is answered by --inputs, which replays the whole
series the kernel build recorded, in order, onto a fresh clone and then this patch, exactly as
scripts/unixnu/patches.py prepare() does; the second by the offsets read back out of the patched file.
The rest are reconciliations between the patch and the sources that have to agree with it: the two
boot arguments the glue reads, the class and the bundle the README records, the counters BootLog's A
row prints, and board.toml's patch_series, whose last entry must be this file's series.

Run: python3 scripts/test-dby-torch-media-mount-patch.py [--inputs <out>/<device>/<os-build>/<board>/xnu-inputs.json>]
"""
import argparse
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCHES = ROOT / 'patches/xnu'
PATCH = PATCHES / 'zze-dby-torch-media-mount.patch'
SERIES = PATCHES / 'series-torch'
GLUE_H = ROOT / 'package/DbyTorch/DbyTorchIos.h'
GLUE_CPP = ROOT / 'package/DbyTorch/DbyTorchIos.cpp'
BOOT_LOG = ROOT / 'package/BootLog.c'
STUBS = ROOT / 'package/BootProgressStubs.c'
BOARD = ROOT / 'board.toml'
README = ROOT / 'package/DbyTorch/README.md'
FILE = 'bsd/kern/kern_exec.c'
INPUTS = None

patch_text = PATCH.read_text()


class MountPatch(unittest.TestCase):
    def test_only_kern_exec_changes(self):
        self.assertEqual(sorted(set(re.findall(r'^\+\+\+ b/(\S+)', patch_text, re.M))), [FILE])
        self.assertEqual(sorted(set(re.findall(r'^--- a/(\S+)', patch_text, re.M))), [FILE])
        self.assertEqual(len(re.findall(r'^@@ ', patch_text, re.M)), 2, 'the patch is two hunks: the counters and the call')

    def test_the_gate_is_the_two_boot_arguments(self):
        self.assertIn('PE_parse_boot_argn("dby-torch-ios", &torch_ios, sizeof(torch_ios)) && torch_ios == 1', patch_text)
        self.assertIn('PE_parse_boot_argn("dby-torch", &torch_mode, sizeof(torch_mode)) && torch_mode == 1', patch_text)
        glue = (ROOT / 'package/DbyTorch/DbyTorch.cpp').read_text()
        for name in ('dby-torch-ios', 'dby-torch'):
            self.assertIn('"%s"' % name, glue, f'the glue does not read {name} either')
            self.assertIn(name, README.read_text(), f'{name} is not in the README table')

    def test_the_names_are_the_ones_the_glue_and_the_record_carry(self):
        header = GLUE_H.read_text()
        service = re.search(r'#define DBT_IOS_SERVICE\s+"([^"]+)"', header).group(1)
        self.assertIn('a service of class %s exists' % service, patch_text, 'the patch probes a different class than the glue registers')
        bundle = re.search(r'\+[ \t]*"(/System/Library/MediaCapture)",[ \t]*"(/System/Library/MediaCapture/[^"]+)"', patch_text)
        self.assertIsNotNone(bundle, 'the mount passes the directory as visible and the bundle as own')
        plugin = bundle.group(2)
        self.assertIn(plugin, README.read_text(), f'{plugin} is not the bundle the README records')
        self.assertIn('"/private/var/ngpu/mediacapture"', patch_text)
        self.assertIn(plugin, patch_text)

    def test_the_commit_call_is_the_ladder_and_the_prototype(self):
        self.assertIn('extern void dby_torch_ios_commit(int plugin_visible) __attribute__((weak_import));', patch_text)
        self.assertIn('dby_torch_ios_commit(native_gpu_addon5_stage == 5);', patch_text)
        self.assertIn('void dby_torch_ios_commit(int plugin_visible);', GLUE_H.read_text())
        self.assertIn('extern "C" void dby_torch_ios_commit(int plugin_visible)', GLUE_CPP.read_text())

    def test_the_ladder_reaches_the_log(self):
        for counter in ('native_gpu_addon5_stage', 'native_gpu_addon5_status'):
            self.assertIn(f'volatile int {counter}', patch_text, f'{counter} is not declared by the patch')
            self.assertIn(f'extern volatile int {counter} __attribute__((weak_import));', BOOT_LOG.read_text())
            self.assertIn(f'__attribute__((weak)) int {counter};', STUBS.read_text())
        self.assertIn('mount5=%d/%d', BOOT_LOG.read_text(), 'the A row does not print the torch mount')
        self.assertIn('mount4=%d/%d', BOOT_LOG.read_text(), 'the A row does not print the audio mount the record names')

    def test_this_series_is_last_and_names_only_this_patch(self):
        names = [line.strip() for line in SERIES.read_text().splitlines() if line.strip() and not line.strip().startswith('#')]
        self.assertEqual(names, [PATCH.name])
        series = re.search(r'patch_series = \[(.*?)\]\n', BOARD.read_text(), re.S).group(1)
        entries = re.findall(r'"([^"]+)"', series)
        self.assertEqual(entries[-1], 'device:patches/xnu/series-torch', 'the torch series must be applied last: its context is the audio patch')
        self.assertEqual(entries[-2], 'device:patches/xnu/series-audio')
        for path in (PATCHES / 'series', PATCHES / 'series-usb', PATCHES / 'series-audio'):
            self.assertNotIn(PATCH.name, path.read_text(), f'{PATCH.name} is named twice in the series')

    def test_the_recorded_series_still_lands_and_this_patch_follows_it(self):
        if not INPUTS:
            self.skipTest('no --inputs xnu-inputs.json given')
        inputs = json.loads(Path(INPUTS).read_text())
        for patch in inputs['patches']:
            self.assertTrue(Path(patch['path']).is_file(), f'{patch["path"]} is not here any more')
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'xnu'
            subprocess.run(['git', 'clone', '--quiet', '--shared', '--no-checkout', inputs['source'], str(root)], check=True)
            subprocess.run(['git', '-C', str(root), 'checkout', '--quiet', '--detach', inputs['commit']], check=True)
            for patch in inputs['patches']:
                result = subprocess.run(['git', '-C', str(root), 'apply', patch['path']], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, f'{patch["path"]}: {result.stderr[-400:]}')
            result = subprocess.run(['git', '-C', str(root), 'apply', str(PATCH)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, f'{PATCH.name}: {result.stderr[-400:]}')
            text = (root / FILE).read_text()
            body = text[text.index('native_gpu_addon_prepare(proc_t p, user_addr_t scratch)'):]
            body = body[:body.index('\nnative_staged_apps_mkdir')]
            offsets = re.findall(r'native_gpu_addon_mount\(p, scratch, (0x[0-9a-f]+),', body)
            self.assertEqual(len(offsets), 5, f'the addon block has {len(offsets)} mounts, not five')
            self.assertEqual(len(set(offsets)), 5, f'two mounts share a scratch window: {offsets}')
            self.assertEqual(offsets[-1], '0xe00', 'the torch mount is not the last one in the block, before the cryptex view')
            self.assertIn('dby_torch_ios_commit(native_gpu_addon5_stage == 5);', body)
            self.assertEqual(subprocess.run(['git', '-C', str(root), 'apply', '--reverse', '--check', str(PATCH)],
                                            capture_output=True, text=True).returncode, 0,
                             'the patched file is not this patch applied and nothing else')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--inputs')
    args, rest = parser.parse_known_args()
    INPUTS = args.inputs
    unittest.main(argv=[sys.argv[0]] + rest)
