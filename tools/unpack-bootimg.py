#!/usr/bin/env python3
"""Unpack an Android boot / init_boot (header v3/v4) or vendor_boot (v3/v4) image into OUTDIR.

    unpack-bootimg.py IMAGE OUTDIR

boot: kernel, ramdisk, header.txt. vendor_boot: dtb, one file per vendor ramdisk fragment (ramdisk-N-NAME),
bootconfig, header.txt. Only reads; the image is not changed.
"""
import os
import struct
import sys


def pages(n, page):
    return (n + page - 1) // page * page


def cstr(b):
    return b.split(b'\0', 1)[0].decode(errors='replace')


def boot(img, out, info):
    kernel_size, ramdisk_size, os_version, header_size = struct.unpack_from('<4I', img, 8)
    header_version, = struct.unpack_from('<I', img, 40)
    cmdline = cstr(img[44:44 + 1536])
    page = 4096
    sig_size = struct.unpack_from('<I', img, 44 + 1536)[0] if header_version >= 4 else 0
    off = page
    k = img[off:off + kernel_size]
    off += pages(kernel_size, page)
    r = img[off:off + ramdisk_size]
    info += ['type boot', f'header_version {header_version}', f'kernel_size {kernel_size}',
             f'ramdisk_size {ramdisk_size}', f'os_version 0x{os_version:x}', f'signature_size {sig_size}',
             f'cmdline {cmdline}']
    if kernel_size:
        open(os.path.join(out, 'kernel'), 'wb').write(k)
    if ramdisk_size:
        open(os.path.join(out, 'ramdisk'), 'wb').write(r)


def vendor_boot(img, out, info):
    header_version, page, kernel_addr, ramdisk_addr, vr_size = struct.unpack_from('<5I', img, 8)
    cmdline = cstr(img[28:28 + 2048])
    o = 28 + 2048
    tags_addr, = struct.unpack_from('<I', img, o)
    name = cstr(img[o + 4:o + 20])
    header_size, dtb_size = struct.unpack_from('<2I', img, o + 20)
    dtb_addr, = struct.unpack_from('<Q', img, o + 28)
    info += ['type vendor_boot', f'header_version {header_version}', f'page_size {page}',
             f'kernel_addr 0x{kernel_addr:x}', f'ramdisk_addr 0x{ramdisk_addr:x}', f'tags_addr 0x{tags_addr:x}',
             f'dtb_addr 0x{dtb_addr:x}', f'name {name}', f'vendor_ramdisk_size {vr_size}', f'dtb_size {dtb_size}',
             f'cmdline {cmdline}']
    off = pages(header_size, page)
    vr = img[off:off + vr_size]
    off += pages(vr_size, page)
    open(os.path.join(out, 'dtb'), 'wb').write(img[off:off + dtb_size])
    off += pages(dtb_size, page)
    if header_version < 4:
        open(os.path.join(out, 'ramdisk-0-default'), 'wb').write(vr)
        return
    t_size, t_num, t_esize, bc_size = struct.unpack_from('<4I', img, o + 36)
    table = img[off:off + t_size]
    off += pages(t_size, page)
    for i in range(t_num):
        e = table[i * t_esize:(i + 1) * t_esize]
        size, offset, typ = struct.unpack_from('<3I', e, 0)
        ename = cstr(e[12:12 + 32])
        info.append(f'ramdisk {i} type {typ} offset {offset} size {size} name {ename!r}')
        open(os.path.join(out, f'ramdisk-{i}-{ename or "default"}'), 'wb').write(vr[offset:offset + size])
    if bc_size:
        open(os.path.join(out, 'bootconfig'), 'wb').write(img[off:off + bc_size])
        info.append(f'bootconfig_size {bc_size}')


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    img = open(sys.argv[1], 'rb').read()
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    info = []
    if img[:8] == b'ANDROID!':
        boot(img, out, info)
    elif img[:8] == b'VNDRBOOT':
        vendor_boot(img, out, info)
    else:
        sys.exit(f'{sys.argv[1]}: not a boot or vendor_boot image (magic {img[:8]!r})')
    open(os.path.join(out, 'header.txt'), 'w').write('\n'.join(info) + '\n')
    print('\n'.join(info))


if __name__ == '__main__':
    main()
