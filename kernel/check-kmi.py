#!/usr/bin/env python3
"""Check that a kernel build can load the U30 Pro's stock vendor modules.

    check-kmi.py Module.symvers DIR...

Every .ko under DIR carries, in __versions, the CRC of each kernel symbol it uses as the kernel it was built against
saw it (CONFIG_MODVERSIONS). The kernel refuses a module whose CRC for a symbol differs from its own ("disagrees
about version of symbol"), or that uses a symbol it does not export. This reads them all and compares them with
the build's Module.symvers; symbols that other modules of the same set export are not the kernel's concern.
Exit status 1 when anything would fail to load.
"""
import os
import struct
import sys


def sections(data):
    if data[:4] != b'\x7fELF' or data[4] != 2:
        return {}
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3a)
    hdrs = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    stroff = hdrs[shstrndx][4]
    out = {}
    for h in hdrs:
        name = data[stroff + h[0]:data.index(b'\0', stroff + h[0])].decode()
        out[name] = data[h[4]:h[4] + h[5]]
    return out


def versions(sec):
    for i in range(0, len(sec), 64):
        crc, = struct.unpack_from('<Q', sec, i)
        name = sec[i + 8:i + 64].split(b'\0', 1)[0].decode()
        yield name, crc & 0xffffffff


def exported(secs):
    # __ksymtab_strings holds the names a module exports
    return set(x.decode() for x in secs.get('__ksymtab_strings', b'').split(b'\0') if x)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    kernel = {}
    for line in open(sys.argv[1]):
        f = line.rstrip('\n').split('\t')
        # vmlinux and the build's own modules (rfkill.ko, which cfg80211 needs, is a GKI module, not vmlinux)
        if len(f) >= 3:
            kernel[f[1]] = int(f[0], 16)
    mods = {}
    for d in sys.argv[2:]:
        for root, _, files in os.walk(d):
            for fn in files:
                if fn.endswith('.ko'):
                    mods.setdefault(fn, os.path.join(root, fn))
    secs = {fn: sections(open(p, 'rb').read()) for fn, p in mods.items()}
    by_modules = set().union(*(exported(s) for s in secs.values()))
    bad = 0
    checked = 0
    for fn in sorted(secs):
        problems = []
        for name, crc in versions(secs[fn].get('__versions', b'')):
            if name in ('module_layout',):
                continue
            checked += 1
            if name in kernel:
                if kernel[name] != crc:
                    problems.append(f'{name}: module 0x{crc:08x}, kernel 0x{kernel[name]:08x}')
            elif name not in by_modules:
                problems.append(f'{name}: not exported')
        if problems:
            bad += 1
            print(f'{fn}:')
            for p in problems[:8]:
                print('   ', p)
            if len(problems) > 8:
                print(f'    ... {len(problems) - 8} more')
    print(f'{len(secs)} modules, {checked} symbol references, {bad} modules would not load')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
