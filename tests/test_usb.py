"""openwrt/usb/mu5358-usb (the U30 Pro's USB tool), boot/init's usb_apply block and the rpcd object mu5358usb, each
against a fake tree: a configfs gadget as a directory (symlinks for the function links, plain files for the
attributes), a fake /sys/class/udc, an etc/, a data/ and a run/ directory, and stubs on PATH for mkfs.vfat, mount and
umount that record their arguments. Runs under the host's sh (macOS: bash as sh; CI: dash); no docker needed.

Every path the script touches has an environment override (its header lists them), so nothing here reaches the
machine's own /sys or /etc."""
import os
import shutil
import stat
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from helpers import TOP

USB = TOP / 'openwrt/usb/mu5358-usb'
UDC = 'musb-hdrc.1.auto'
RECORDER = '''#!/bin/sh
# records its name and arguments, one call a line, NUL between the arguments; prints a canned reply when one is set
printf '%s' "$(basename "$0")" >> "$STUBLOG"; for a in "$@"; do printf '\\0%s' "$a" >> "$STUBLOG"; done; printf '\\n' >> "$STUBLOG"
[ -z "${STUB_REPLY:-}" ] || printf '%s' "$STUB_REPLY"
exit "${STUB_RC:-0}"
'''


class Base(unittest.TestCase):
    """A fake gadget (NCM + ACM bound to the UDC), the directories and the stubs; `usb` runs the script."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='mu5358-usb-'))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.g = self.tmp / 'g'
        for d in ('functions/ncm.usb0', 'functions/acm.GS0', 'configs/c.1/strings/0x409', 'strings/0x409'):
            (self.g / d).mkdir(parents=True)
        os.symlink(self.g / 'functions/ncm.usb0', self.g / 'configs/c.1/f1')
        os.symlink(self.g / 'functions/acm.GS0', self.g / 'configs/c.1/f2')
        (self.g / 'configs/c.1/strings/0x409/configuration').write_text('ncm + ACM console\n')
        (self.g / 'UDC').write_text(UDC + '\n')
        (self.g / 'idProduct').write_text('0xa4a1\n')
        (self.g / 'bcdDevice').write_text('0x0301\n')
        self.udc = self.tmp / 'udc'
        (self.udc / UDC).mkdir(parents=True)
        self.data, self.etc, self.run_dir, self.bin, self.mnt = (self.tmp / n for n in ('data', 'etc', 'run', 'bin', 'mnt'))
        for d in (self.data, self.etc, self.bin):
            d.mkdir()
        self.log = self.tmp / 'stub.log'
        for name in ('mkfs.vfat', 'mount', 'umount'):
            self.stub(name)
        self.env = dict(os.environ, PATH=f'{self.bin}:{os.environ["PATH"]}', STUBLOG=str(self.log),
                        MU5358_GADGET=str(self.g), MU5358_UDC_CLASS=str(self.udc),
                        MU5358_STORAGE_CONF=str(self.etc / 'usb-storage'), MU5358_USB_DISK=str(self.data / 'usb-disk.img'),
                        MU5358_RUN=str(self.run_dir), MU5358_MNT=str(self.mnt), MU5358_REBIND_PAUSE='0')

    def stub(self, name, reply='', rc=0):
        p = self.bin / name
        p.write_text(RECORDER)
        p.chmod(0o755)
        return p

    def usb(self, *args, env=None, stdin=None):
        e = dict(self.env, **(env or {}))
        return subprocess.run(['sh', str(USB), *args], env=e, capture_output=True, text=True, input=stdin, timeout=60)

    def calls(self):
        """[[name, arg, ...], ...] every stub call so far."""
        if not self.log.exists():
            return []
        return [line.split('\0') for line in self.log.read_text().splitlines() if line]

    def attr(self, rel):
        return (self.g / rel).read_text().strip()

    def rebound(self):
        return (self.run_dir / 'usb-rebound').exists()

    def image(self):
        return self.data / 'usb-disk.img'


class Storage(Base):
    def test_on_adds_the_function_and_rebinds(self):
        r = self.usb('storage', 'on')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(os.readlink(self.g / 'configs/c.1/f3'), str(self.g / 'functions/mass_storage.0'))
        self.assertEqual(self.attr('functions/mass_storage.0/lun.0/removable'), '1')
        self.assertEqual(self.attr('functions/mass_storage.0/lun.0/nofua'), '1')
        self.assertEqual(self.attr('functions/mass_storage.0/lun.0/file'), str(self.image()))
        self.assertEqual(self.attr('idProduct'), '0xa4a2')
        self.assertEqual(self.attr('configs/c.1/strings/0x409/configuration'), 'ncm + ACM console + USB disk')
        self.assertEqual(self.attr('UDC'), UDC)
        self.assertTrue(self.rebound())
        self.assertEqual((self.etc / 'usb-storage').read_text(), 'on 2048\n')
        self.assertEqual(os.stat(self.image()).st_size, 2048 << 20)
        mkfs = [c for c in self.calls() if c[0] == 'mkfs.vfat']
        self.assertEqual(len(mkfs), 1, self.calls())
        self.assertIn('-F', mkfs[0]); self.assertIn('32', mkfs[0]); self.assertEqual(mkfs[0][-1], str(self.image()))
        self.assertIn('on', r.stdout)

    def test_on_twice_is_quiet_and_does_not_rebind(self):
        self.usb('storage', 'on')
        (self.run_dir / 'usb-rebound').unlink()
        n = len(self.calls())
        r = self.usb('storage', 'on')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(self.rebound())
        self.assertEqual(len(self.calls()), n, 'mkfs ran again')
        self.assertEqual(self.attr('UDC'), UDC)

    def test_off_ejects_removes_and_restores_identity(self):
        self.usb('storage', 'on')
        (self.run_dir / 'usb-rebound').unlink()
        r = self.usb('storage', 'off')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse((self.g / 'configs/c.1/f3').is_symlink())
        self.assertEqual(self.attr('idProduct'), '0xa4a1')
        self.assertEqual(self.attr('configs/c.1/strings/0x409/configuration'), 'ncm + ACM console')
        self.assertEqual(self.attr('UDC'), UDC)
        self.assertTrue(self.rebound())
        self.assertEqual((self.etc / 'usb-storage').read_text(), 'off 2048\n')
        self.assertTrue(self.image().exists(), 'the image is kept')

    def test_size_follows_the_free_space(self):
        r = self.usb('storage', 'on', env={'MU5358_FREE_KB': str(2560 * 1024)})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(os.stat(self.image()).st_size, 1536 << 20)
        self.assertEqual((self.etc / 'usb-storage').read_text(), 'on 1536\n')

    def test_refused_under_256_mib(self):
        r = self.usb('storage', 'on', env={'MU5358_FREE_KB': str(1228 * 1024)})
        self.assertEqual(r.returncode, 1)
        self.assertIn('256 MiB', r.stderr)
        self.assertFalse(self.image().exists())
        self.assertFalse((self.etc / 'usb-storage').exists())
        self.assertFalse((self.g / 'configs/c.1/f3').is_symlink())

    def test_size_only_while_off(self):
        self.usb('storage', 'on')
        r = self.usb('storage', 'size', '512')
        self.assertEqual(r.returncode, 1)
        self.assertIn('turn it off first', r.stderr)
        self.assertEqual(os.stat(self.image()).st_size, 2048 << 20)
        self.usb('storage', 'off')
        r = self.usb('storage', 'size', '512')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(os.stat(self.image()).st_size, 512 << 20)
        self.assertEqual((self.etc / 'usb-storage').read_text(), 'off 512\n')
        self.assertEqual(self.usb('storage', 'size', '12abc').returncode, 2)
        self.assertEqual(self.usb('storage', 'size', '32').returncode, 1)
        self.assertEqual(self.usb('storage', 'size', '9999', env={'MU5358_FREE_KB': str(4096 * 1024)}).returncode, 1)

    def test_toggle_without_a_bound_gadget_saves_the_state(self):
        (self.g / 'UDC').write_text('\n')
        shutil.rmtree(self.udc / UDC)
        r = self.usb('storage', 'on')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stderr, '')
        self.assertEqual((self.etc / 'usb-storage').read_text(), 'on 2048\n')
        self.assertTrue((self.g / 'configs/c.1/f3').is_symlink())
        self.assertFalse(self.rebound())

    def test_status_lines(self):
        r = self.usb('storage', 'status', env={'MU5358_FREE_KB': str(3000 * 1024)})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('storage: off\n', r.stdout)
        self.assertIn('size: 2048 MiB\n', r.stdout)
        self.assertIn('exported: no\n', r.stdout)
        self.assertIn(f'image: {self.image()} (missing)\n', r.stdout)
        self.assertIn('free: 3000 MiB\n', r.stdout)
        self.usb('storage', 'on')
        r = self.usb('storage', 'status')
        self.assertIn('storage: on\n', r.stdout)
        self.assertIn('exported: yes\n', r.stdout)
        self.assertIn(f'image: {self.image()} (present)\n', r.stdout)

    def test_mount_ejects_first_and_umount_reinserts(self):
        self.usb('storage', 'on')
        r = self.usb('storage', 'mount')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.attr('functions/mass_storage.0/lun.0/file'), '')
        mount = [c for c in self.calls() if c[0] == 'mount']
        self.assertEqual(len(mount), 1, self.calls())
        self.assertIn('-o', mount[0]); self.assertIn('loop', ','.join(mount[0]))
        self.assertEqual(mount[0][-2:], [str(self.image()), str(self.mnt)])
        r = self.usb('storage', 'umount')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual([c for c in self.calls() if c[0] == 'umount'][0][-1], str(self.mnt))
        self.assertEqual(self.attr('functions/mass_storage.0/lun.0/file'), str(self.image()))

    def test_usage(self):
        for args in (('storage',), ('storage', 'bogus')):
            r = self.usb(*args)
            self.assertEqual(r.returncode, 2, args)
            self.assertIn('usage', r.stderr)


if __name__ == '__main__':
    unittest.main()
