#!/usr/bin/env python3
"""patch-props.py DIR: the chroot's copy of Android's property area with liblog let through.

ZTE's Android sets persist.log.tag to S (silent): with it, modem_control and cp_diskserver log nothing at all under
Linux. A prop_info is serial (4 bytes, the value's length in the top byte), value (92 bytes), name; the value here
becomes V, the same length. Only the copy in the chroot is changed (2026-10-29)."""
import glob, os, sys

name = b'persist.log.tag\0'
done = 0
for f in glob.glob(os.path.join(sys.argv[1], '*')):
    b = bytearray(open(f, 'rb').read())
    i = b.find(name)
    if i < 96:
        continue
    v = i - 92
    if b[v - 1] != 1 or b[v:v + 2] not in (b'S\0', b'V\0'):
        sys.exit(f'{f}: unexpected value at {v}: {bytes(b[v - 4:v + 4])}')
    b[v] = ord('V')
    open(f, 'wb').write(b)
    done += 1
    print(f'{f}: persist.log.tag = V')
sys.exit(0 if done else 'persist.log.tag not found')
