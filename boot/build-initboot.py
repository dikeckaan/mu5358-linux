#!/usr/bin/env python3
"""Build the Linux init_boot image for the U30 Pro's other slot, and the two misc blocks that go with it.

    build-initboot.py --stock init_boot_a.img --busybox busybox --init boot/init --misc misc.img --out DIR

DIR/init_boot.img   the stock init_boot image with its ramdisk replaced by ours (header v4, kernel_size 0): the
                    stock image's AVB data (vbmeta blob and footer) is moved behind the new ramdisk, as Magisk does
                    with the image this device boots today. Same size as the partition.
The ramdisk also carries /mu/module-order: vendor_boot's recovery module list (--vendor-modules DIR, its
lib/modules) in dependency order, which modules.load.recovery is not (sdhci-sprd comes before cqhci there: the
second trial on b had no eMMC); and --extra KO files (from vendor_dlkm) in /mu/modules, loaded after them.

DIR/boot.img        with --kernel IMAGE and --stock-boot boot_a.img: the stock boot image with its kernel replaced
                    (kernel/build.sh's Image), its AVB data moved behind it the same way

DIR/misc-bc-android.bin    the live bootloader_control of misc (Android on a): written back by our init
DIR/misc-bc-b-trial.bin    the same with b at priority 15 and 2 tries (Unisoc LK takes tries == 1 && !successful as a
                    boot that already failed), a at 14 and still successful: one trial of b, then a again
"""
import argparse
import os
import stat
import struct
import subprocess
import zlib

PAGE = 4096
BC_OFF = 0x800


def cpio_record(name, data, mode, ino, rdev=(0, 0)):
    name_b = name.encode() + b'\0'
    hdr = b'070701' + b''.join(b'%08X' % v for v in (
        ino, mode, 0, 0, 1, 0, len(data), 0, 0, rdev[0], rdev[1], len(name_b), 0))
    rec = hdr + name_b
    rec += b'\0' * (-len(rec) % 4)
    rec += data
    rec += b'\0' * (-len(rec) % 4)
    return rec


def ramdisk(files, dirs):
    out = b''
    ino = 1000
    for d in dirs:
        out += cpio_record(d, b'', stat.S_IFDIR | 0o755, ino)
        ino += 1
    for name, (data, mode) in files.items():
        out += cpio_record(name, data, mode, ino)
        ino += 1
    # the kernel opens /dev/console before it runs /init
    out += cpio_record('dev/console', b'', stat.S_IFCHR | 0o600, ino, (5, 1))
    out += cpio_record('TRAILER!!!', b'', 0, ino + 1)
    # LZ4 legacy, like vendor_boot's ramdisk in front of it: the kernel's unlz4 reads the segment after an LZ4
    # legacy one as more LZ4 chunks, so a gzip segment there is never unpacked (our first trial on b, 2026-10-29;
    # mu300-linux FINDINGS 2)
    return subprocess.run(['lz4', '-l', '-12', '-c'], input=out, stdout=subprocess.PIPE, check=True).stdout


def module_order(vdir, exclude=()):
    """modules.load.recovery, every module after the ones it depends on (modules.dep), each once; without the
    modules of EXCLUDE and every module that depends on one of them"""
    dep = {}
    for line in open(os.path.join(vdir, 'modules.dep')):
        k, v = line.split(':', 1)
        dep[os.path.basename(k)] = [os.path.basename(x) for x in v.split()]
    out = set(exclude)
    while True:
        more = {m for m, d in dep.items() if m not in out and out & set(d)}
        if not more:
            break
        out |= more
    order, seen = [], set(out)

    def visit(m):
        if m in seen:
            return
        seen.add(m)
        for d in dep.get(m, []):
            visit(d)
        order.append(m)
    for line in open(os.path.join(vdir, 'modules.load.recovery')):
        if line.strip():
            visit(line.strip())
    return order


def bc_blocks(misc):
    bc = misc[BC_OFF:BC_OFF + 32]
    if bc[4:8] != b'BCAB':
        raise SystemExit('misc has no bootloader_control at 0x800')
    if zlib.crc32(bc[:28]) != struct.unpack('<I', bc[28:])[0]:
        raise SystemExit('misc bootloader_control CRC mismatch')
    if bc[0:2] != b'_a':
        raise SystemExit(f'Android is not on slot a (suffix {bc[0:4]!r}); this build only does Linux on b')
    # slot byte: priority (bits 0-3) | tries (4-6) | successful (7)
    a = bc[12]
    if a & 0x0f != 15 or not a & 0x80:
        raise SystemExit(f'slot a is not priority 15 and successful (0x{a:02x})')
    x = bytearray(bc)
    x[0:4] = b'_b\0\0'
    x[12] = (a & 0xf0) | 14
    x[14] = 0x2f
    x[28:32] = struct.pack('<I', zlib.crc32(bytes(x[:28])))
    return bytes(bc), bytes(x)


def initboot(stock, rd):
    return replace_payload(stock, rd, 'ramdisk')


def replace_payload(stock, payload, what):
    """the v4 boot image STOCK with its kernel or its ramdisk (what) replaced by PAYLOAD, the other one absent"""
    img = bytearray(stock)
    if img[:8] != b'ANDROID!' or struct.unpack_from('<I', img, 40)[0] != 4:
        raise SystemExit('stock image is not a v4 boot image')
    kernel_size, ramdisk_size = struct.unpack_from('<2I', img, 8)
    if (what == 'ramdisk' and kernel_size) or (what == 'kernel' and ramdisk_size):
        raise SystemExit(f'stock image has a {"kernel" if kernel_size else "ramdisk"} besides its {what}')
    foot = img[-64:]
    if foot[:4] != b'AVBf':
        raise SystemExit('stock image has no AVB footer')
    rd = payload
    _osz, vo, vs = struct.unpack('>QQQ', foot[12:36])
    vbmeta = bytes(img[vo:vo + vs])
    end = PAGE + (len(rd) + PAGE - 1) // PAGE * PAGE
    new_vo = end
    if new_vo + vs > len(img) - PAGE:
        raise SystemExit('ramdisk too large for the partition')
    out = bytearray(len(img))
    out[:PAGE] = img[:PAGE]
    struct.pack_into('<I', out, 8 if what == 'kernel' else 12, len(rd))
    out[PAGE:PAGE + len(rd)] = rd
    out[new_vo:new_vo + vs] = vbmeta
    nf = bytearray(foot)
    struct.pack_into('>QQQ', nf, 12, end, new_vo, vs)
    out[-64:] = nf
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--stock', required=True)
    ap.add_argument('--busybox', required=True)
    ap.add_argument('--init', required=True)
    ap.add_argument('--misc', required=True, help='misc partition image (at least its first 4 KiB)')
    ap.add_argument('--vendor-modules', required=True, help="vendor_boot ramdisk's lib/modules")
    ap.add_argument('--extra', nargs='*', default=[], help='more modules, loaded after vendor_boot\'s')
    # audio-misc: procd's udevtrigger writes "add" to its vbc_turning* devices' uevent, and the class name it formats
    # is in freed memory - an oops a few seconds into OpenWrt, every few boots (2026-10-29). The board plays no sound.
    ap.add_argument('--exclude', nargs='*', default=['audio-misc.ko'],
                    help='modules left out with everything that depends on them (default: audio-misc.ko)')
    ap.add_argument('--kernel', help='kernel Image for boot.img (with --stock-boot)')
    ap.add_argument('--stock-boot', help='stock boot image (boot_a), for --kernel')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    android, trial = bc_blocks(open(a.misc, 'rb').read(PAGE))
    files = {
        'init': (open(a.init, 'rb').read(), stat.S_IFREG | 0o755),
        'mu/busybox': (open(a.busybox, 'rb').read(), stat.S_IFREG | 0o755),
        'mu/misc-bc-android.bin': (android, stat.S_IFREG | 0o644),
    }
    files['mu/module-order'] = (''.join(m + '\n' for m in module_order(a.vendor_modules, a.exclude)).encode(),
                                stat.S_IFREG | 0o644)
    for ko in a.extra:
        files['mu/modules/' + os.path.basename(ko)] = (open(ko, 'rb').read(), stat.S_IFREG | 0o644)
    rd = ramdisk(files, ['mu', 'mu/modules'])
    img = initboot(open(a.stock, 'rb').read(), rd)
    for name, data in (('init_boot.img', img), ('misc-bc-android.bin', android), ('misc-bc-b-trial.bin', trial)):
        open(os.path.join(a.out, name), 'wb').write(data)
    print(f'ramdisk {len(rd)} bytes; init_boot.img {len(img)} bytes; {len(module_order(a.vendor_modules, a.exclude))} modules')
    if a.kernel:
        k = open(a.kernel, 'rb').read()
        if k[0x38:0x3c] != b'ARM\x64':
            raise SystemExit(f'{a.kernel}: not an arm64 Image')
        boot = replace_payload(open(a.stock_boot, 'rb').read(), k, 'kernel')
        open(os.path.join(a.out, 'boot.img'), 'wb').write(boot)
        print(f'kernel {len(k)} bytes; boot.img {len(boot)} bytes')
    print(f'misc android  {android.hex()}')
    print(f'misc b trial  {trial.hex()}')


if __name__ == '__main__':
    main()
