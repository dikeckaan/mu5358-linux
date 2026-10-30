#!/usr/bin/env python3
"""Unpack the sysdumpdb partition the Unisoc bootloader writes after an abnormal reset (a minidump).

The partition holds a text index ("NNN_minidump_<name>:<size> -1", dump_report.txt, ...) and the sections as zlib
streams. Every zlib stream found is decompressed into OUTDIR/<offset>.bin, and each one is named after the index
entry whose (compressed) size matches, when exactly one does.

usage: sysdumpdb-extract.py sysdumpdb.bin OUTDIR
"""
import os
import re
import sys
import zlib

src, out = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
os.makedirs(out, exist_ok=True)

index = {}
for m in re.finditer(rb'([A-Za-z0-9_.]{3,64}):(\d+) -?\d+', data):
    index.setdefault(int(m.group(2)), []).append(m.group(1).decode())
for size, names in sorted(index.items()):
    print(f'index {size:9} {" ".join(names)}')

i = 0
while i < len(data) - 2:
    if data[i] == 0x78 and (data[i] * 256 + data[i + 1]) % 31 == 0:
        d = zlib.decompressobj()
        try:
            raw = d.decompress(data[i:i + 32 * 1024 * 1024])
        except zlib.error:
            i += 1
            continue
        if d.eof and len(raw) >= 16:
            used = len(data[i:i + 32 * 1024 * 1024]) - len(d.unused_data)
            names = index.get(used, [])
            name = names[0] if len(names) == 1 else ''
            path = os.path.join(out, f'{i:08x}{"-" + name if name else ""}.bin')
            open(path, 'wb').write(raw)
            print(f'{i:#010x} {used:8} -> {len(raw):9} {name or "?"}')
            i += used
            continue
    i += 1
