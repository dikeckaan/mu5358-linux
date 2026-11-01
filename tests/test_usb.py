"""openwrt/usb/mu5358-usb (the U30 Pro's USB tool), boot/init's usb_apply block and the rpcd object mu5358usb, each
against a fake tree: a configfs gadget as a directory (symlinks for the function links, plain files for the
attributes), a fake /sys/class/udc, an etc/, a data/ and a run/ directory, and stubs on PATH for mkfs.vfat, mount and
umount that record their arguments. Runs under the host's sh (macOS: bash as sh; CI: dash); no docker needed.

Every path the script touches has an environment override (its header lists them), so nothing here reaches the
machine's own /sys or /etc."""
import json
import os
import shutil
import stat
import subprocess
import re
import tempfile
import time
import unittest
from pathlib import Path

from helpers import TOP

USB = TOP / 'openwrt/usb/mu5358-usb'
INIT = TOP / 'boot/init'
RPCD = TOP / 'openwrt/usb/rpcd-mu5358usb'
ACL = TOP / 'openwrt/usb/acl-mu5358usb.json'
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
        for d in ('functions/ncm.usb0', 'functions/acm.GS0', 'configs/c.1/strings/0x409', 'strings/0x409', 'os_desc'):
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
        # configfs refuses a function link into a configuration while the gadget is bound (ln: Invalid argument,
        # seen on the bench 2026-11-01): the stand-in ln does the same
        ln = self.bin / 'ln'
        ln.write_text('#!/bin/sh\ncase "$*" in *configs/c.1/f*) [ -z "$(cat "$MU5358_GADGET/UDC" 2>/dev/null)" ] || '
                      '{ echo "ln: $2: Invalid argument" >&2; exit 1; } ;; esac\nexec /bin/ln "$@"\n')
        ln.chmod(0o755)
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

    def test_free_space_counted_where_the_image_will_live(self):
        shutil.rmtree(self.data)   # no /data yet: the first "on" makes it, and the free space is its parent's
        r = self.usb('storage', 'status')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn('free: 0 MiB', r.stdout)
        r = self.usb('storage', 'on')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(self.image().exists())

    def test_on_and_size_refuse_while_the_image_is_mounted_on_the_device(self):
        mounts = self.tmp / 'mounts'
        mounts.write_text(f'/dev/loop0 {self.mnt} vfat rw,relatime 0 0\n')
        for args in (('storage', 'on'), ('storage', 'size', '512')):
            r = self.usb(*args, env={'MU5358_PROC_MOUNTS': str(mounts)})
            self.assertEqual(r.returncode, 1, args)
            self.assertIn('umount first', r.stderr)
        self.assertFalse((self.g / 'configs/c.1/f3').is_symlink())
        self.assertFalse((self.etc / 'usb-storage').exists())
        self.assertFalse(self.image().exists())

    def test_a_failure_after_the_unbind_binds_the_gadget_back(self):
        funcs = self.g / 'functions'
        funcs.chmod(0o555)   # mkdir of the disk function fails, as on a kernel without it
        self.addCleanup(funcs.chmod, 0o755)
        r = self.usb('storage', 'on')
        self.assertEqual(r.returncode, 1)
        self.assertIn('cannot make', r.stderr)
        self.assertEqual(self.attr('UDC'), UDC, 'the gadget was left off the controller')
        self.assertTrue(self.rebound())
        self.assertFalse((self.etc / 'usb-storage').exists())

    def test_usage(self):
        for args in (('storage',), ('storage', 'bogus')):
            r = self.usb(*args)
            self.assertEqual(r.returncode, 2, args)
            self.assertIn('usage', r.stderr)


class InitApply(Base):
    """boot/init's usb_apply block: the composite the system's state files ask for, before switch_root."""
    HARNESS = """
log() {{ echo "LOG $*"; }}
g={g}
UDC_CLASS={udc}
# configfs makes lun.0 and os_desc/interface.rndis itself; here mkdir does
mkdir() {{
    command mkdir "$@" || return 1
    for _d in "$@"; do case $_d in -*) ;; */mass_storage.0) command mkdir -p "$_d/lun.0" ;; */rndis.rn0) command mkdir -p "$_d/os_desc/interface.rndis" ;; esac; done
}}
rmdir() {{ rm -rf "$@"; }}
{block}
usb_apply {root}
"""

    def block(self):
        text = INIT.read_text(encoding='utf-8')
        m = re.search(r'# --- usb-apply begin\n(.*?)# --- usb-apply end\n', text, re.S)
        self.assertIsNotNone(m, 'boot/init has no "# --- usb-apply begin/end" block')
        return m.group(1)

    def apply(self, net=None, storage=None, once=False, data=True):
        root = self.tmp / 'root'
        (root / 'etc/mu5358').mkdir(parents=True)
        if data:
            (root / 'data').mkdir()
        if net is not None:
            (root / 'etc/mu5358/usb-net').write_text(net + '\n')
        if once:
            (root / 'etc/mu5358/usb-net.once').write_text('')
        if storage is not None:
            (root / 'etc/mu5358/usb-storage').write_text(storage + '\n')
        script = self.HARNESS.format(g=self.g, udc=self.udc, block=self.block(), root=root)
        r = subprocess.run(['sh', '-c', script], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stderr, '')
        self.root = root
        return r.stdout

    def f(self, n):
        p = self.g / f'configs/c.1/f{n}'
        return os.path.basename(os.readlink(p)) if p.is_symlink() else None

    def applied(self):
        return (self.root / 'etc/mu5358/usb-net-applied').read_text().strip()

    def test_ncm_and_no_disk_is_what_is_bound_no_rebind(self):
        out = self.apply()
        self.assertEqual((self.f(1), self.f(2), self.f(3)), ('ncm.usb0', 'acm.GS0', None))
        self.assertEqual(self.applied(), 'ncm')
        self.assertIn('stage=usb-apply net=ncm disk=0 rebind=0', out)
        self.assertEqual(self.attr('UDC'), UDC)

    def test_rndis_rebuilds_f1_with_the_ms_os_descriptors(self):
        out = self.apply(net='rndis')
        self.assertEqual((self.f(1), self.f(2)), ('rndis.rn0', 'acm.GS0'))
        self.assertFalse((self.g / 'functions/ncm.usb0').exists())
        self.assertEqual(self.attr('bcdDevice'), '0x0302')
        self.assertEqual(self.attr('os_desc/use'), '1')
        self.assertEqual(self.attr('os_desc/qw_sign'), 'MSFT100')
        self.assertEqual(self.attr('os_desc/b_vendor_code'), '0xcd')
        self.assertEqual(self.attr('functions/rndis.rn0/os_desc/interface.rndis/compatible_id'), 'RNDIS')
        self.assertEqual(self.attr('functions/rndis.rn0/os_desc/interface.rndis/sub_compatible_id'), '5162001')
        self.assertEqual(os.readlink(self.g / 'os_desc/c.1'), str(self.g / 'configs/c.1'))
        self.assertEqual(self.attr('functions/rndis.rn0/host_addr'), '02:35:58:00:00:04')
        self.assertEqual(self.attr('functions/rndis.rn0/dev_addr'), '02:35:58:00:00:03')
        self.assertEqual(self.attr('configs/c.1/strings/0x409/configuration'), 'RNDIS + ACM console')
        self.assertIn('rebind=1', out)
        self.assertEqual(self.attr('UDC'), UDC)
        self.assertEqual(self.applied(), 'rndis')

    def test_ecm_rebuilds_f1(self):
        self.apply(net='ecm')
        self.assertEqual(self.f(1), 'ecm.usb0')
        self.assertEqual(self.attr('configs/c.1/strings/0x409/configuration'), 'ecm + ACM console')
        self.assertEqual(self.attr('functions/ecm.usb0/host_addr'), '02:35:58:00:00:02')
        self.assertEqual(self.attr('bcdDevice'), '0x0301')

    def test_storage_on_adds_f3_without_a_medium(self):
        out = self.apply(storage='on 2048')
        self.assertEqual((self.f(1), self.f(2), self.f(3)), ('ncm.usb0', 'acm.GS0', 'mass_storage.0'))
        self.assertEqual(self.attr('functions/mass_storage.0/lun.0/removable'), '1')
        self.assertEqual(self.attr('functions/mass_storage.0/lun.0/nofua'), '1')
        f = self.g / 'functions/mass_storage.0/lun.0/file'
        self.assertTrue(not f.exists() or f.read_text().strip() == '')
        self.assertEqual(self.attr('idProduct'), '0xa4a2')
        self.assertEqual(self.attr('configs/c.1/strings/0x409/configuration'), 'ncm + ACM console + USB disk')
        self.assertIn('net=ncm disk=1 rebind=1', out)

    def test_storage_on_without_an_image_still_adds_the_function(self):
        self.apply(storage='on 2048', data=False)
        self.assertEqual(self.f(3), 'mass_storage.0')

    def test_garbage_in_the_files_means_the_defaults(self):
        out = self.apply(net='wat', storage='maybe 12')
        self.assertEqual((self.f(1), self.f(3)), ('ncm.usb0', None))
        self.assertEqual(self.applied(), 'ncm')
        self.assertIn('rebind=0', out)

    def test_once_is_not_the_init_s_business(self):
        self.apply(net='ecm', once=True)
        self.assertEqual(self.f(1), 'ecm.usb0')
        self.assertTrue((self.root / 'etc/mu5358/usb-net.once').exists())

    def test_place_in_init(self):
        text = INIT.read_text(encoding='utf-8')
        self.assertEqual(text.count('usb_apply /newroot'), 1)
        self.assertLess(text.index('/mu5358-stay'), text.index('usb_apply /newroot'))
        self.assertLess(text.index('usb_apply /newroot'), text.index('kill $timer $console'))


class Boot(Base):
    """mu5358-usb boot: the medium in, the image made when missing, the applied mode copied; never a rebind."""

    def disk_function(self):
        lun = self.g / 'functions/mass_storage.0/lun.0'
        lun.mkdir(parents=True)
        (lun / 'file').write_text('\n')
        os.symlink(self.g / 'functions/mass_storage.0', self.g / 'configs/c.1/f3')
        return lun

    def test_inserts_the_medium_when_on_and_the_function_is_there(self):
        lun = self.disk_function()
        (self.etc / 'usb-storage').write_text('on 2048\n')
        self.image().write_bytes(b'x')
        r = self.usb('boot')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((lun / 'file').read_text().strip(), str(self.image()))
        self.assertFalse(self.rebound())
        self.assertEqual([c for c in self.calls() if c[0] == 'mkfs.vfat'], [])

    def test_makes_the_image_when_missing(self):
        lun = self.disk_function()
        (self.etc / 'usb-storage').write_text('on 2048\n')
        r = self.usb('boot')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len([c for c in self.calls() if c[0] == 'mkfs.vfat']), 1)
        self.assertEqual(os.stat(self.image()).st_size, 2048 << 20)
        self.assertEqual((lun / 'file').read_text().strip(), str(self.image()))

    def test_without_the_function_it_says_so_and_does_not_rebind(self):
        (self.etc / 'usb-storage').write_text('on 2048\n')
        r = self.usb('boot')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('update from the page once', r.stdout + r.stderr)
        self.assertFalse((self.g / 'configs/c.1/f3').is_symlink())
        self.assertFalse(self.rebound())

    def test_copies_the_applied_mode_to_run(self):
        (self.etc / 'usb-net-applied').write_text('rndis\n')
        r = self.usb('boot', env={'MU5358_APPLIED': str(self.etc / 'usb-net-applied')})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((self.run_dir / 'usb-net-applied').read_text().strip(), 'rndis')

    def test_off_does_nothing(self):
        (self.etc / 'usb-storage').write_text('off 2048\n')
        r = self.usb('boot')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(self.image().exists())
        self.assertEqual(self.calls(), [])


class Net(Base):
    def setUp(self):
        super().setUp()
        self.stub('device-usb')
        self.stub('usb-adb')
        self.env.update(MU5358_DEVICE_USB=str(self.bin / 'device-usb'), MU5358_USB_ADB=str(self.bin / 'usb-adb'),
                        MU5358_USBNET=str(self.etc / 'usb-net'), MU5358_APPLIED=str(self.etc / 'usb-net-applied'))

    def test_delegates_to_device_usb(self):
        for args, want in ((('net', 'rndis', 'once'), ['set-net', 'rndis', 'once', '1']),
                           (('net', 'ecm'), ['set-net', 'ecm', 'permanent', '1']),
                           (('net', 'default'), ['set-net', 'ncm', 'permanent', '0'])):
            self.log.unlink(missing_ok=True)
            r = self.usb(*args)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(self.calls(), [['device-usb', *want]], args)
        self.assertEqual(self.usb('net', 'bogus').returncode, 2)

    def test_shows_the_choice(self):
        (self.etc / 'usb-net').write_text('ecm\n')
        (self.etc / 'usb-net.once').write_text('')
        (self.etc / 'usb-net-applied').write_text('ncm\n')
        r = self.usb('net')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('ecm (once)', r.stdout)
        self.assertIn('this boot: ncm', r.stdout)
        self.assertEqual(self.calls(), [])

    def test_adb_delegates(self):
        r = self.usb('adb', 'key', '-', stdin='QAAAA...=\n')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.calls(), [['usb-adb', 'adb', 'key', '-']])


class Host(Base):
    def setUp(self):
        super().setUp()
        self.typec = self.tmp / 'typec'
        self.typec.mkdir()
        (self.typec / 'data_role').write_text('[device] host\n')
        self.env.update(MU5358_TYPEC=str(self.typec), MU5358_HOST_UNDO='1')

    def role(self):
        return (self.typec / 'data_role').read_text().strip()

    def test_host_writes_the_data_role_and_undoes_itself(self):
        r = self.usb('host')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.role(), 'host')
        self.assertIn('usb-host-keep', r.stdout)
        time.sleep(2.5)
        self.assertEqual(self.role(), 'device')

    def test_keep_file_cancels_the_undo(self):
        self.usb('host', env={'MU5358_HOST_UNDO': '3'})
        time.sleep(1)
        self.run_dir.mkdir(exist_ok=True)
        (self.run_dir / 'usb-host-keep').write_text('')   # the tester's decision, after the swap
        time.sleep(3)
        self.assertEqual(self.role(), 'host')

    def test_a_stale_keep_file_does_not_cancel_the_undo(self):
        # /run/mu5358 outlived a reboot on the bench (2026-11-01): a keep file from an earlier test must not count
        self.run_dir.mkdir()
        (self.run_dir / 'usb-host-keep').write_text('')
        self.usb('host')
        self.assertFalse((self.run_dir / 'usb-host-keep').exists())
        time.sleep(2.5)
        self.assertEqual(self.role(), 'device')

    def test_device_puts_it_back(self):
        self.run_dir.mkdir()
        (self.run_dir / 'usb-host-keep').write_text('')
        (self.typec / 'data_role').write_text('[host] device\n')
        r = self.usb('device')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.role(), 'device')
        self.assertFalse((self.run_dir / 'usb-host-keep').exists())

    def test_no_typec_is_refused(self):
        r = self.usb('host', env={'MU5358_TYPEC': str(self.tmp / 'nowhere')})
        self.assertEqual(r.returncode, 1)
        self.assertIn('no USB-C port controller', r.stderr)
        self.assertEqual(self.role(), '[device] host')


class Rpcd(Base):
    """The rpcd object mu5358usb (list, call get, call set) over a recorded stand-in for mu5358-usb."""
    STATUS = ('storage: on\nsize: 2048 MiB\nexported: yes\nimage: /data/usb-disk.img (present)\nfree: 1234 MiB\n'
              'USB network from the next boot: ncm (the default)\nthis boot: ncm\nadb: on\nauth: keys\nrole: device\n')

    def setUp(self):
        super().setUp()
        self.tool = self.stub('mu5358-usb')
        self.env.update(MU5358_USB_BIN=str(self.tool), STUB_REPLY=self.STATUS)

    def rpc(self, *args, stdin=None):
        r = subprocess.run(['sh', str(RPCD), *args], env=self.env, capture_output=True, text=True, input=stdin, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        return json.loads(r.stdout)

    def test_list(self):
        self.assertEqual(self.rpc('list'), {'get': {}, 'set': {'kind': 'str', 'value': 'str'}})

    def test_get(self):
        self.assertEqual(self.rpc('call', 'get'), {'ok': 1, 'storage': 'on', 'size_mib': 2048, 'exported': 1, 'image': 1,
                                                   'free_mib': 1234, 'adb': 'keys', 'role': 'device'})
        self.assertEqual(self.calls(), [['mu5358-usb', 'status']])

    def test_set_storage_runs_the_tool(self):
        out = self.rpc('call', 'set', stdin='{"kind":"storage","value":"on"}')
        self.assertEqual(out['ok'], 1)
        self.assertEqual(self.calls(), [['mu5358-usb', 'storage', 'on']])
        self.assertIn('message', out)

    def test_set_size(self):
        self.rpc('call', 'set', stdin='{ "kind": "storage_size", "value": "512" }')
        self.assertEqual(self.calls(), [['mu5358-usb', 'storage', 'size', '512']])
        self.log.unlink()
        self.rpc('call', 'set', stdin='{"kind":"adb","value":"off"}')
        self.assertEqual(self.calls(), [['mu5358-usb', 'adb', 'off']])

    def test_refuses_what_is_not_allowed(self):
        for body in ('{"kind":"storage","value":"on; reboot"}', '{"kind":"reboot","value":"on"}',
                     '{"kind":"storage_size","value":"-5"}', '{"kind":"storage_size","value":"1e3"}', '{"value":"on"}', 'garbage'):
            out = self.rpc('call', 'set', stdin=body)
            self.assertEqual(out['ok'], 0, body)
            self.assertIn('error', out)
        self.assertEqual(self.calls(), [])

    def test_a_refusal_of_the_tool_is_an_error(self):
        out = self.rpc('call', 'set', stdin='{"kind":"storage","value":"on"}')
        self.assertEqual(out['ok'], 1)
        self.env.update(STUB_RC='1', STUB_REPLY='')
        out = self.rpc('call', 'set', stdin='{"kind":"storage","value":"on"}')
        self.assertEqual(out['ok'], 0)

    def test_acl_names_the_methods(self):
        acl = json.loads(ACL.read_text())
        self.assertEqual(acl['luci-app-mu5358']['read']['ubus']['mu5358usb'], ['get'])
        self.assertEqual(acl['luci-app-mu5358']['write']['ubus']['mu5358usb'], ['set'])


if __name__ == '__main__':
    unittest.main()
