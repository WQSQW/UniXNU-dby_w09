#!/usr/bin/env python3
"""Host tests of the legacy APR audio packet router (package/tests/apr_audio_host.c): the byte layout of every request this board's
Kona stack sends - core handshake, AVCS service versions, AFE clock/TDM/slot mapping/port start, ADM route, ASM stream - plus the
decoder's malformed-packet rules and the pending table. Model tests only; nothing here touches hardware."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'apr-host'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
                    str(PACKAGE / 'tests/apr_audio_host.c'), str(PACKAGE / 'ShengApr.c'), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
sys.stderr.write(result.stderr)
print(result.stdout.strip())
sys.exit(0 if result.returncode == 0 and result.stdout.startswith('PASS') else 1)
