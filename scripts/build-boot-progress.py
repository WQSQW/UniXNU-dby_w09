#!/usr/bin/env python3
"""Build the opt-in board diagnostic against this source kernel's actual ABI."""
import hashlib
import os
import json
from pathlib import Path
import subprocess


def warning_demotions(command):
    """Diagnostic classes the kernel pass does not treat as errors, so neither should the kit.

    The kernel's own recorded command carries no -Werror; these kit commands add it on top and
    throw away every -W flag from the recorded command, so the compiler's warnings about xnu's own
    headers (offsetof's null-pointer subtraction, the 'driverkit' availability platform the
    IOKit headers use) become fatal. Probe each name so a compiler that does not know it just
    drops the flag, the way build-platform-power.py:109 does.
    """
    # Probe with the triple the real compile uses: the official cross clang has no default
    # target, and a bare syntax check fails with "unknown target triple 'unknown'" before it
    # can judge the flag, which would silently drop every demotion.
    target = [arg for arg in reversed(command) if arg.startswith('--target=')]
    names = ['-Wno-error=null-pointer-subtraction', '-Wno-error=availability']
    keep = []
    for name in names:
        probe = subprocess.run([str(command[0]), *target, '-Werror=unknown-warning-option', name,
                                '-x', 'c', '-fsyntax-only', '-'], input='', capture_output=True)
        if probe.returncode == 0:
            keep.append(name)
    return keep
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/boot-progress'
OUT.mkdir(parents=True, exist_ok=True)
uefi = Path(os.path.expanduser(os.environ.get('UNIXNU_UEFI') or str(ROOT.parent / 'uefi-sheng')))
fb_lib = uefi / 'Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib'
extra_includes = ['-I', str(fb_lib)] if fb_lib.is_dir() else []
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(','))
source = ROOT / 'kits/dby_w09/BootProgress.c'
obj = OUT / 'BootProgress.o'
command = []
args = record['arguments']
i = 0
while i < len(args):
    arg = args[i]
    if arg == record['file']:
        command.append(str(source))
    elif arg in ('-o', '-MF', '-MT', '-MJ'):
        if arg == '-o':
            command += ['-o', str(obj)]
        i += 1
    elif arg == '-Xclang' and args[i+1].startswith('-W'):
        i += 1
    elif arg == '-D' and args[i+1].startswith('XNU_BOOT_PROGRESS='):
        i += 1
    elif not arg.startswith(('-W', '-DXNU_BOOT_PROGRESS=')):
        command.append(arg)
    i += 1
command += ['-DXNU_BOOT_PROGRESS=1', '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
            *warning_demotions(command),
            '-I', str(ROOT / '.cache/xnu-work/osfmk'),
            *extra_includes]
if '-o' not in command:
    # The recorded Linux compile command has no -o, so without this clang writes the object into
    command += ['-o', str(obj)]
subprocess.run(command, cwd=record['directory'], check=True, timeout=90)
(OUT / 'compile-command.json').write_text(json.dumps(command,indent=2)+'\n')
fault_source = ROOT / 'kits/dby_w09/BootFaultProbe.c'
fault_obj = OUT / 'BootFaultProbe.o'
fault_command = [str(fault_source) if arg == str(source) else str(fault_obj) if arg == str(obj) else arg for arg in command]
subprocess.run(fault_command, cwd=record['directory'], check=True, timeout=90)
policy_source = ROOT / 'kits/dby_w09/BootWatchdogPolicy.c'
policy_obj = OUT / 'BootWatchdogPolicy.o'
policy_record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json').read_text().strip().rstrip(','))
policy_command = []
policy_args = policy_record['arguments']
j = 0
while j < len(policy_args):
    arg = policy_args[j]
    if arg == policy_record['file']:
        policy_command.append(str(policy_source))
    elif arg in ('-o', '-MF', '-MT', '-MJ'):
        if arg == '-o': policy_command += ['-o', str(policy_obj)]
        j += 1
    elif arg == '-Xclang' and policy_args[j+1].startswith('-W'):
        j += 1
    elif not arg.startswith('-W'):
        policy_command.append(arg)
    j += 1
policy_command += ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
                   *warning_demotions(policy_command)]
if '-o' not in policy_command:
    policy_command += ['-o', str(policy_obj)]
subprocess.run(policy_command, cwd=policy_record['directory'], check=True, timeout=90)

# Official userland diagnostics (package/BootProcessProbe.c, package/BootUiTraceProbe.c). They were
# copied into the board package but never compiled, so boot_ui_trace_ready stayed the zero stub from
# BootProgressStubs.c and the documented UI stack trace could never draw. Compile them with the same
# osfmk startup.o.json command (it carries -I <tree>/.cache/xnu-work/osfmk for the kern/vm/ipc
# source headers); the package directory supplies BootUiTraceCore.h.
probe_objs = []
for probe_name in ('BootProcessProbe.c', 'BootUiTraceProbe.c'):
    probe_source = source.parent / probe_name
    probe_obj = OUT / (probe_source.stem + '.o')
    probe_command = [str(probe_source) if arg == str(source) else str(probe_obj) if arg == str(obj) else arg for arg in command]
    probe_command += ['-I', str(source.parent)]
    subprocess.run(probe_command, cwd=policy_record['directory'], check=True, timeout=120)
    probe_objs.append(str(probe_obj))

archive = OUT / 'libBootProgress.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj), str(fault_obj), str(policy_obj), *probe_objs], check=True, timeout=30)
(OUT / 'build-report.json').write_text(json.dumps({
    'watchdog_policy_sha256': hashlib.sha256(policy_source.read_bytes()).hexdigest(),
    'fault_sha256': hashlib.sha256(fault_source.read_bytes()).hexdigest(),
    'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'lifecycle_sha256': hashlib.sha256((source.parent / 'BootProgressLifecycle.h').read_bytes()).hexdigest(),
    'core_sha256': hashlib.sha256((source.parent / 'BootProgressCore.h').read_bytes()).hexdigest(),
    'display_sha256': hashlib.sha256((source.parent / 'BootProgressDisplay.h').read_bytes()).hexdigest(),
    'pulse_sha256': hashlib.sha256((source.parent / 'BootProgressPulse.h').read_bytes()).hexdigest(),
    'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
    'hardware_execution': False,
}, indent=2) + '\n')
print('PASS: strict source-kernel ARM64 boot diagnostic build')
