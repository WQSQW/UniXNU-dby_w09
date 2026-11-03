#!/usr/bin/env python3
"""Pull the NT36523 touch image out of the stock AFE HAL, where this model keeps it.

usage: extract-afe-firmware.py [--list] [--out PATH]

The MatePad 11 ships no standalone TP firmware file. The panel's Novatek image lives inside the per-panel AFE hal,
vendor/lib64/libafehalW<project>.so, as three symbols: NT36523_THP_BIN (the bytes), NT36523_THP_BIN_LEN and
NT36523_THP_CHECKSUM. Those .so files are recorded in proprietary-files.txt and land in .cache/firmware through
scripts/extract-files.py; this script reads them and writes the image beside them, which is what
scripts/gen-touch-firmware.py takes as its argument when the build is meant to carry a downloadable image.

Read-only analysis of files already on disk: nothing is fetched and nothing is written to a device. Each candidate is
reported with the constants that say which panel it is for, so the unit's own answers choose the file rather than this
script guessing: guiCHIPID 0x23650300 is the trim id this part answers with (the vendor trim_id_table row
{0x20,0xFF,0xFF,0x23,0x65,0x03}, novatek_thp.c:193), guiDRAMCMDADDR 0x2FE50 is the command mailbox the kernel's N9Z
series uses (ic_info_table, novatek_thp.c:110-122), and PROJECT_ID is the panel's project string.
"""
import argparse
import hashlib
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / '.cache/firmware'
HALS = CACHE / 'vendor/lib64'
OUT_DIR = CACHE / 'novatek'
CANDIDATES = ('libafehalW2599Z1100.so', 'libafehalW2599Z1300.so', 'libafehalW259AX1100.so')
WORDS = ('NT36523_THP_BIN', 'NT36523_THP_BIN_LEN', 'NT36523_THP_CHECKSUM', 'PROJECT_ID', 'guiCHIPID',
         'guiDRAMCMDADDR', 'guiDRAMINFOBUF', 'guiDRAMWrPARAS', 'guiTX_SIZE', 'guiRX_SIZE', 'guiREGBLDCRCEN')


def loads(data):
    """PT_LOAD list of an ELF64 file as (vaddr, file offset, size), for mapping a symbol address into the file."""
    e_phoff, = struct.unpack_from('<Q', data, 32)
    e_phentsize, e_phnum = struct.unpack_from('<HH', data, 54)
    out = []
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        p_type, = struct.unpack_from('<I', data, o)
        p_offset, p_vaddr = struct.unpack_from('<QQ', data, o + 8)
        p_filesz, = struct.unpack_from('<Q', data, o + 32)
        if p_type == 1 and p_filesz:
            out.append((p_vaddr, p_offset, p_filesz))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--list', action='store_true', help='report the candidates and write nothing')
    parser.add_argument('--out', help='write one image here (with more than one candidate, name it per project)')
    args = parser.parse_args()

    written = 0
    for name in CANDIDATES:
        path = HALS / name
        if not path.is_file():
            print(f'skip     {name}: not in {path.parent} - run scripts/extract-files.py with the stock package as SOURCE')
            continue
        data = path.read_bytes()
        segments = loads(data)
        sym = {}
        for line in subprocess.run(['nm', '-D', '--defined-only', str(path)],
                                   capture_output=True, text=True).stdout.splitlines():
            parts = line.split()
            if len(parts) == 3:
                try:
                    sym[parts[2]] = int(parts[0], 16)
                except ValueError:
                    pass

        def word(key):
            vaddr = sym.get(key)
            if vaddr is None:
                return None
            for va, off, size in segments:
                if va <= vaddr < va + size:
                    return struct.unpack_from('<I', data, off + (vaddr - va))[0]
            return None

        base, length = word('NT36523_THP_BIN'), word('NT36523_THP_BIN_LEN')
        if base is None or not length:
            print(f'skip     {name}: no NT36523_THP_BIN, so this hal is for another controller family')
            continue
        blob = b''
        for va, off, size in segments:
            if va <= base < va + size and off + (base - va) + length <= len(data):
                blob = data[off + (base - va): off + (base - va) + length]
                break
        if len(blob) != length:
            print(f'FAILED   {name}: the image at {hex(base)} + {length} does not lie inside the file')
            return 1
        def text(key, width=32):
            # PROJECT_ID is an int symbol, but the string it names is longer than four bytes: the full project id is
            # what the vendor's thp_project_id_match_rules.xml matches on, so read the characters, not the word.
            vaddr = sym.get(key)
            if vaddr is None:
                return ''
            for va, off, size in segments:
                if va <= vaddr < va + size:
                    raw = data[off + (vaddr - va): off + (vaddr - va) + width]
                    return raw.split(b'\0', 1)[0].decode('ascii', 'replace')
            return ''
        project = text('PROJECT_ID') or 'unknown'
        print(f'{name}: project "{project}" chipid {word("guiCHIPID"):#010x} mailbox {word("guiDRAMCMDADDR"):#08x} '
              f'cmd tx/rx {word("guiTX_SIZE")}/{word("guiRX_SIZE")} info {word("guiDRAMINFOBUF"):#08x}')
        print(f'         image {length} bytes sha256 {hashlib.sha256(blob).hexdigest()}')
        print(f'         container checksum field {word("NT36523_THP_CHECKSUM"):#010x}; the bytes add to '
              f'{sum(blob) & 0xffffffff:#010x}, so that field is a second function over the image and is reported, '
              f'not used to gate anything')
        if args.list:
            continue
        target = Path(args.out) if args.out and len(CANDIDATES) == 1 else OUT_DIR / (name[:-3] + '.bin')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(blob)
        print(f'extracted {target.relative_to(ROOT)} ({length} bytes)')
        written += 1
    if args.list or written:
        return 0
    print('FAILED   no candidate in the cache')
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
