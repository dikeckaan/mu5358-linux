"""W2 (docs/superpowers/plans/2026-11-02-updater-from-linux.md): the pieces around the Linux-side updater that run as
their own scripts - mu5358-boot-ok (boot/ok and boot/tries), mu5358-update-notice (the timer loop). Run under the
host's sh against a temporary directory as the filesystem, with sleep, logger and mu5358-update stubbed on PATH."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

TOP = Path(__file__).resolve().parent.parent
UP = TOP / 'openwrt/update'

SLEEP = '''#!/bin/sh
echo "sleep $1" >> "$STUBLOG"
n=$(grep -c '^sleep' "$STUBLOG"); [ "$n" -lt "${SLEEPS:-100}" ] || kill -TERM $PPID
exit 0
'''
LOGGER = '#!/bin/sh\necho "logger $*" >> "$STUBLOG"\n'
UPDATE = '#!/bin/sh\necho "update $*" >> "$STUBLOG"\nexit ${UPDATE_RC:-0}\n'


class Stubbed(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='mu5358-update-'))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.disk = self.tmp / 'disk'
        (self.disk / 'boot').mkdir(parents=True)
        self.log = self.tmp / 'log'
        self.bin = self.tmp / 'bin'
        self.bin.mkdir()
        for name, body in (('sleep', SLEEP), ('logger', LOGGER), ('mu5358-update', UPDATE)):
            p = self.bin / name
            p.write_text(body)
            p.chmod(0o755)
        self.etc = self.tmp / 'etc'
        self.etc.mkdir()

    def run_script(self, name, env=None):
        e = dict(os.environ, PATH=f'{self.bin}:{os.environ["PATH"]}', STUBLOG=str(self.log), MU5358_DISK=str(self.disk),
                 MU5358_ETC=str(self.etc), **(env or {}))
        r = subprocess.run(['sh', str(UP / name)], env=e, capture_output=True, text=True, timeout=30)
        return r, (self.log.read_text() if self.log.exists() else '')


class BootOk(Stubbed):
    def test_boot_ok_writes_ok_and_zeroes_tries(self):
        (self.disk / 'boot/tries').write_text('2\n')
        (self.etc / 'version').write_text('v2026.11.02.1\n')
        r, log = self.run_script('mu5358-boot-ok')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((self.disk / 'boot/ok').read_text(), 'v2026.11.02.1\n')
        self.assertEqual((self.disk / 'boot/tries').read_text().strip(), '0')
        self.assertIn('sleep 30\n', log)
        self.assertIn('logger -t mu5358-boot-ok', log)

    def test_a_fallback_boot_becomes_permanent(self):
        # review #4: booted from system.old (boot/init's fallback), boot-ok swaps it in for good; the tree that failed
        # three times is kept as system.failed
        for name, v in (('system', 'v2026.11.02.2'), ('system.old', 'v2026.11.02.1')):
            (self.disk / name / 'etc/mu5358').mkdir(parents=True)
            (self.disk / name / 'etc/mu5358/version').write_text(v + '\n')
        (self.disk / 'boot/tries').write_text('3\n')
        (self.etc / 'version').write_text('v2026.11.02.1\n')
        r, log = self.run_script('mu5358-boot-ok', env={'MU5358_ROOT': str(self.disk / 'system.old')})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((self.disk / 'system/etc/mu5358/version').read_text().strip(), 'v2026.11.02.1')
        self.assertEqual((self.disk / 'system.failed/etc/mu5358/version').read_text().strip(), 'v2026.11.02.2')
        self.assertFalse((self.disk / 'system.old').exists())
        self.assertEqual((self.disk / 'boot/ok').read_text().strip(), 'v2026.11.02.1')
        self.assertEqual((self.disk / 'boot/tries').read_text().strip(), '0')
        self.assertIn('permanent', log)

    def test_a_flat_install_has_no_boot_dir_and_nothing_is_written(self):
        shutil.rmtree(self.disk / 'boot')
        (self.etc / 'version').write_text('v2026.11.02.1\n')
        r, log = self.run_script('mu5358-boot-ok')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse((self.disk / 'boot').exists())


class Notice(Stubbed):
    def test_the_loop_waits_five_minutes_then_asks_every_six_hours(self):
        r, log = self.run_script('mu5358-update-notice', env={'SLEEPS': '3'})
        self.assertEqual(log.split('\n')[:4], ['sleep 300', 'update notice', 'sleep 21600', 'update notice'])

    def test_a_failed_check_is_tried_again_after_an_hour(self):
        r, log = self.run_script('mu5358-update-notice', env={'SLEEPS': '2', 'UPDATE_RC': '1'})
        self.assertEqual(log.split('\n')[:3], ['sleep 300', 'update notice', 'sleep 3600'])

    def test_update_check_off_ends_the_loop_without_asking(self):
        (self.etc / 'update-check').write_text('off\n')
        r, log = self.run_script('mu5358-update-notice', env={'SLEEPS': '5'})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn('update notice', log)
        self.assertNotIn('sleep', log, 'off is checked before the first sleep (review #10)')


if __name__ == '__main__':
    unittest.main()


# ---- the updater itself: under busybox in docker, against a fake filesystem, release and partitions ---------------
import json
import re
import struct
import sys

sys.path.insert(0, str(TOP / 'tests'))
from helpers import BUSYBOX_IMAGE, DOCKER, docker_busybox, shell_scripts  # noqa: E402

UPDATER = '/w/openwrt/update/mu5358-update'
T1, T2, T0 = 'v2026.11.02.1', 'v2026.11.02.2', 'v2026.11.01.9'


def v4_image(payload, which, total=8 * 4096):
    """A v4 boot image like the device's: a 4 KiB header (ANDROID!, the payload's size, header version 4), the
    payload on a page, a 256-byte vbmeta blob on the next page, and an AVB footer page naming where the blob is."""
    hdr = bytearray(4096)
    hdr[0:8] = b'ANDROID!'
    struct.pack_into('<I', hdr, 8 if which == 'kernel' else 12, len(payload))
    struct.pack_into('<I', hdr, 40, 4)
    pl = payload + b'\0' * (-len(payload) % 4096)
    vb = (b'VB' * 128)
    body = bytes(hdr) + pl + vb + b'\0' * (4096 - len(vb))
    body += b'\0' * (total - 4096 - len(body))
    footer = bytearray(64)
    footer[0:4] = b'AVBf'
    struct.pack_into('>Q', footer, 20, 4096 + len(pl))
    struct.pack_into('>Q', footer, 28, len(vb))
    return body + b'\0' * (4096 - 64) + bytes(footer)


PRELUDE = r'''
set -e
S=''' + UPDATER + r'''
mkdir -p /t/bin /disk/system/sbin /disk/system/etc/mu5358 /disk/system/etc/config /disk/boot /disk/data \
    /t/run /t/motd /t/rd/modules /t/api/releases
mkdir -m 700 -p /t/lock
printf '#!/bin/sh\n' > /disk/system/sbin/init; chmod 755 /disk/system/sbin/init
echo ''' + T1 + r''' > /disk/system/etc/mu5358/version; cp /disk/system/etc/mu5358/version /disk/system/etc/mu5358/image-version
echo keepme > /disk/system/etc/config/mine
echo 'mu300-image-manifest 1 openwrt-luci ''' + T1 + r'''' | gzip > /disk/system/etc/mu5358/image-manifest.gz
mkdir -p /etc/mu5358 && cp /disk/system/etc/mu5358/image-version /etc/mu5358/image-version
echo system > /disk/boot/tree; echo 2 > /disk/boot/tries
{ echo "version=''' + T1 + r'''"; echo "target=region"; echo "layout=tree"; } > /disk/boot/installed
cp /bin/busybox /t/rd/busybox; printf BC > /t/rd/misc-bc-android.bin; printf KO > /t/rd/modules/sprd_pmic_wdt.ko
echo 'console=ttyS1 androidboot.slot_suffix=_b' > /t/cmdline
echo 'console=ttyS1 androidboot.slot_suffix=_a' > /t/cmdline_a
echo region > /t/target
cat > /t/bin/curl <<'X'
#!/bin/sh
out=; url=
while [ $# -gt 0 ]; do case $1 in -o) out=$2; shift ;; -*) ;; *) url=$1 ;; esac; shift; done
f=${url#file://}
[ -f "$f" ] || exit 22
if [ -n "$out" ]; then cat "$f" >> "$out"; else cat "$f"; fi
X
cat > /t/bin/lz4 <<'X'
#!/bin/sh
mode=c; in=; out=
for a; do case $a in -d) mode=d ;; -*) ;; *) if [ -z "$in" ]; then in=$a; else out=$a; fi ;; esac; done
if [ $mode = c ]; then { printf '\002!L\030'; cat "$in"; } > "$out"; else tail -c +5 "$in" > "$out"; fi
X
printf '#!/bin/sh\necho mu5358\n' > /t/bin/mu5358-device
chmod 755 /t/bin/*
mkrel() {  # mkrel TAG: the release's files in /t/rel, the API's "latest" naming it
    _tag=$1; _d=/t/rel; rm -rf $_d; mkdir -p $_d/tree/sbin $_d/tree/etc/mu5358 $_d/tree/etc/config $_d/rd/mu
    printf '#!/bin/sh\n' > $_d/tree/sbin/init; chmod 755 $_d/tree/sbin/init
    echo $_tag > $_d/tree/etc/mu5358/version; echo $_tag > $_d/tree/etc/mu5358/image-version
    echo release > $_d/tree/etc/config/mine
    (cd $_d/tree && tar -czf ../mu5358-system.tar.gz .)
    { head -c 56 /dev/zero; printf 'ARMd'; printf '%s' "KERNEL-$_tag"; head -c 64 /dev/zero; } > $_d/Image
    gzip -c $_d/Image > $_d/mu5358-kernel.Image.gz
    printf '#!/bin/sh\n' > $_d/rd/init; cp /bin/busybox $_d/rd/mu/busybox
    (cd $_d/rd && tar -czf ../mu5358-ramdisk.tar.gz init mu)
    cp $S $_d/mu5358-update
    echo $_tag > $_d/mu5358-release
    (cd $_d && { echo "# version $_tag"; sha256sum mu5358-system.tar.gz mu5358-kernel.Image.gz mu5358-ramdisk.tar.gz mu5358-update mu5358-release; } > SHA256SUMS)
    printf '{"tag_name": "%s"}\n' $_tag > /t/api/releases/latest
}
export MU5358_DISK=/disk MU5358_LOCK=/t/lock MU5358_NOTICE=/t/run/update-available \
    MU5358_MOTD_DIR=/t/motd MU5358_BIN=/t/bin MU5358_BYNAME=/t/parts MU5358_RAMDISK=/t/rd MU5358_CMDLINE=/t/cmdline \
    MU5358_LZ4=/t/bin/lz4 MU5358_ROOT_TARGET=/t/target MU5358_RELEASE_URL=file:///t/rel MU5358_API_URL=file:///t/api \
    MU5358_FETCH_DELAY=0 PATH=/t/bin:$PATH
set +e
'''


class Updater(unittest.TestCase):
    """mu5358-update (openwrt/update/mu5358-update): apply swaps system/ and system.old/ and writes the boot images
    with the previous ones kept in data/prev; rollback and rollback-boot undo them; status is JSON; the release's
    checksums must name the tag (mu5358-release); a flat installation and a downgrade are refused."""

    @classmethod
    def setUpClass(cls):
        why = docker_busybox()
        if why:
            raise unittest.SkipTest(why)

    def setUp(self):
        self.t = Path(tempfile.mkdtemp(prefix='mu5358-updater-', dir=TOP / 'work'))
        self.addCleanup(shutil.rmtree, self.t, ignore_errors=True)
        parts = self.t / 'parts'
        parts.mkdir()
        (parts / 'boot_a').write_bytes(v4_image(b'ANDROID-KERNEL', 'kernel'))
        (parts / 'init_boot_a').write_bytes(v4_image(b'ANDROID-RAMDISK', 'ramdisk', total=4 << 20))
        (parts / 'boot_b').write_bytes(v4_image(b'OLD-LINUX-KERNEL', 'kernel'))
        (parts / 'init_boot_b').write_bytes(v4_image(b'OLD-LINUX-RAMDISK', 'ramdisk', total=4 << 20))
        self.old_boot_b = (parts / 'boot_b').read_bytes()

    def sh(self, script, timeout=180):
        # the filesystem lives inside the container (/disk: mknod fails on a host mount) and is copied out at the end
        tail = '\n_rc=$?; rm -rf /t/disk; cp -a /disk /t/disk 2>/dev/null; exit $_rc\n'
        argv = [DOCKER, 'run', '--rm', '-v', f'{TOP}:/w:ro', '-v', f'{self.t}:/t', BUSYBOX_IMAGE, 'sh', '-c', PRELUDE + script + tail]
        return subprocess.run(argv, capture_output=True, text=True, timeout=timeout)

    def version(self, tree='system'):
        return (self.t / 'disk' / tree / 'etc/mu5358/version').read_text().strip()

    def installed(self, key):
        m = re.search(rf'^{key}=(.*)$', (self.t / 'disk/boot/installed').read_text(), re.M)
        return m.group(1) if m else None

    def test_apply_swaps_the_trees_and_writes_the_boot_images(self):
        r = self.sh(f'mkrel {T2}; sh $S apply')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.version(), T2)
        self.assertEqual(self.version('system.old'), T1)
        self.assertEqual((self.t / 'disk/system/etc/config/mine').read_text().strip(), 'keepme', 'the user\'s file carried')
        self.assertEqual(self.installed('version'), T2)
        self.assertEqual(self.installed('target'), 'region')
        boot_b = (self.t / 'parts/boot_b').read_bytes()
        self.assertEqual(boot_b[4096 + 56:4096 + 60], b'ARMd')
        self.assertIn(f'KERNEL-{T2}'.encode(), boot_b)
        self.assertEqual((self.t / 'disk/data/prev/boot_b.img').read_bytes(), self.old_boot_b)
        self.assertIn(b'\x02!L\x18', (self.t / 'parts/init_boot_b').read_bytes())
        self.assertEqual((self.t / 'disk/boot/tries').read_text().strip(), '0')
        self.assertFalse((self.t / 'disk/boot/ok').exists())
        self.assertIn('start Linux again from the web installer', r.stdout)
        self.assertFalse((self.t / 'disk/.mu5358-update').exists(), 'the stage is gone after a whole update')

    def test_apply_refuses_a_flat_install(self):
        r = self.sh(f'mkrel {T2}; mv /disk/system/* /disk/ && rmdir /disk/system; sh $S apply')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('flat installation', r.stderr)
        self.assertIn('web installer', r.stderr)

    def test_apply_refuses_a_downgrade_without_the_flag(self):
        r = self.sh(f'mkrel {T0}; sh $S apply; echo "rc=$?"; MU5358_ALLOW_DOWNGRADE=1 sh $S apply >/dev/null 2>&1; echo "rc2=$?"')
        self.assertIn('rc=1', r.stdout)
        self.assertIn('--downgrade', r.stderr)
        self.assertIn('rc2=0', r.stdout)
        self.assertEqual(self.version(), T0)

    def test_sums_for_another_tag_refused(self):
        # the files of T2 (whose SHA256SUMS lists mu5358-release = sha256 of "T2\n") served as the newest release T3
        r = self.sh(f'mkrel {T2}; printf \'{{"tag_name": "v2026.11.02.3"}}\\n\' > /t/api/releases/latest; sh $S apply')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('checksums of another release', r.stderr)
        self.assertEqual(self.version(), T1)
        self.assertEqual((self.t / 'parts/boot_b').read_bytes(), self.old_boot_b)

    def test_rollback_swaps_back_and_restores_the_boot_images(self):
        r = self.sh(f'mkrel {T2}; sh $S apply >/dev/null 2>&1; sh $S rollback')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.version(), T1)
        self.assertFalse((self.t / 'disk/system.old').exists())
        self.assertFalse((self.t / 'disk/system.broken').exists(), 'the newer copy is not the running one: removed')
        self.assertEqual((self.t / 'parts/boot_b').read_bytes(), self.old_boot_b)
        self.assertEqual(self.installed('version'), T1)
        self.assertIn('starts Android', r.stdout)

    def test_rollback_boot_alone_restores_prev(self):
        r = self.sh(f'mkrel {T2}; sh $S apply >/dev/null 2>&1; sh $S rollback-boot')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual((self.t / 'parts/boot_b').read_bytes(), self.old_boot_b)
        self.assertEqual(self.installed('version'), T1)
        self.assertEqual(self.version(), T2, 'the tree stays')
        self.assertFalse((self.t / 'disk/data/prev/boot_b.img').exists())

    def test_apply_power_cut_between_swap_and_boot(self):
        # the boot step refused (Linux not from slot b): the tree is swapped, the record is behind; status says
        # pending; the next apply of the same tag writes only the boot images
        r = self.sh(f'mkrel {T2}; MU5358_CMDLINE=/t/cmdline_a sh $S apply; echo "rc=$?"; sh $S status; sh $S apply; echo "rc=$?"; sh $S status')
        self.assertIn('The boot images were not updated', r.stdout)
        self.assertEqual(self.version(), T2)
        self.assertEqual(self.installed('version'), T2)
        statuses = [json.loads(l) for l in r.stdout.splitlines() if l.startswith('{"ok":1')]
        self.assertEqual([s['boot_pending'] for s in statuses], [1, 0], r.stdout)
        self.assertEqual(statuses[0]['boot'], T1)
        self.assertEqual(statuses[1]['boot'], T2)
        self.assertIn(f'KERNEL-{T2}'.encode(), (self.t / 'parts/boot_b').read_bytes())

    def test_the_record_keeps_the_installers_target(self):
        # review #2: root_layout answered "region" for every device; a fulldumpdb installation then lost its system
        # on the Android side. boot/init publishes the target; without it the record's own target stands.
        r = self.sh(f'mkrel {T2}; sed -i "s/^target=.*/target=fulldumpdb/" /disk/boot/installed; rm -f /t/target; unset MU5358_ROOT_TARGET; sh $S apply >/dev/null 2>&1; echo "rc=$?"')
        self.assertIn('rc=0', r.stdout, r.stderr)
        self.assertEqual(self.installed('target'), 'fulldumpdb')

    def test_a_small_filesystem_with_room_for_the_tree_updates(self):
        # review #3: mu300's "1 GB + 1 GB a system" rule refused fulldumpdb (2 GiB, two trees, 1.3 GB free)
        r = self.sh(f'mkrel {T2}; printf \'#!/bin/sh\\necho "Filesystem 1024-blocks Used Available Capacity Mounted"\\necho "/dev/x 2000000 700000 600000 54%% /disk"\\n\' > /t/bin/df; chmod 755 /t/bin/df; sh $S apply; echo "rc=$?"')
        self.assertIn('rc=0', r.stdout, r.stdout[-800:] + r.stderr[-800:])
        self.assertEqual(self.version(), T2)

    def test_the_carry_runs_with_the_base_trees_tools(self):
        # review #6: the suite only ever took the settings-keep fallback. With the base tree's mu300-carry and its
        # static busybox in the fake release tree (the base tarball is the maintainer's; docker must be aarch64) the
        # real carry runs: the user's file carried by the manifest, the report written, the fallback line absent.
        base = Path('/Volumes/mu300rel12/r30/release/v2026.10.29.1/mu300-openwrt-luci-rootfs.tar.gz')
        if not base.exists():
            self.skipTest('no base rootfs tarball')
        arch = subprocess.run([DOCKER, 'run', '--rm', BUSYBOX_IMAGE, 'uname', '-m'], capture_output=True, text=True).stdout.strip()
        if arch != 'aarch64':
            self.skipTest(f'docker runs {arch}, the carry binary is aarch64')
        tools = self.t / 'tools'
        tools.mkdir()
        subprocess.run(['tar', '-xzf', str(base), '-C', str(tools), './opt/mu300/bin/mu300-carry', './opt/mu300/bin/busybox'], check=True)
        r = self.sh(f'mkrel {T2}; T=/t/rel/tree; mkdir -p $T/opt/mu5358/bin $T/opt/mu5358/lib && cp /t/tools/opt/mu300/bin/mu300-carry $T/opt/mu5358/bin/mu5358-carry && '
                    f'cp /t/tools/opt/mu300/bin/busybox $T/opt/mu5358/bin/busybox && cp $S $T/opt/mu5358/bin/mu5358-update && chmod 755 $T/opt/mu5358/bin/*; '
                    f'$T/opt/mu5358/bin/mu5358-carry manifest $T openwrt-luci {T2} | gzip > $T/etc/mu5358/image-manifest.gz; '
                    f'(cd $T && tar -czf ../mu5358-system.tar.gz .); (cd /t/rel && {{ echo "# version {T2}"; sha256sum mu5358-system.tar.gz mu5358-kernel.Image.gz mu5358-ramdisk.tar.gz mu5358-update mu5358-release; }} > SHA256SUMS); '
                    f'mkdir -p /disk/system/opt/mu5358/bin && cp /t/tools/opt/mu300/bin/mu300-carry /disk/system/opt/mu5358/bin/mu5358-carry; '
                    f'/disk/system/opt/mu5358/bin/mu5358-carry manifest /disk/system openwrt-luci {T1} | gzip > /disk/system/etc/mu5358/image-manifest.gz; '
                    f'echo mine > /disk/system/etc/config/mine; echo added > /disk/system/root/added 2>/dev/null || {{ mkdir -p /disk/system/root && echo added > /disk/system/root/added; }}; '
                    f'sh $S apply; echo "rc=$?"')
        self.assertIn('rc=0', r.stdout, r.stdout[-1500:] + r.stderr[-800:])
        self.assertNotIn('does not bring what carries everything over', r.stdout)
        self.assertNotIn('no list of the files', r.stdout, 'the manifest of the old tree is read (its header is the image\'s name)')
        self.assertIn('carrying your files over', r.stdout)
        self.assertEqual((self.t / 'disk/system/etc/config/mine').read_text().strip(), 'mine')
        self.assertEqual((self.t / 'disk/system/root/added').read_text().strip(), 'added')
        self.assertTrue((self.t / 'disk/system/etc/mu5358/update-report').exists())

    def test_the_carry_reads_the_manifest_of_the_one_system(self):
        # the bench (2026-11-02): carry_image answered the system's name ("system") and the manifest header says
        # the image's (openwrt-luci), so the carry ran without the list and could not tell a changed file from one
        # the old release shipped
        r = self.sh(f'mkdir -p /tmp/new /tmp/old/etc/mu5358 && {{ echo "mu300-image-manifest 1 openwrt-luci {T1}"; echo "f 644 0 0 5 eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee etc/x"; }} | gzip > /tmp/old/etc/mu5358/image-manifest.gz; '
                    f'MU5358_LIB=1 . $S; carry_image /tmp/old system; image_kind mu5358-system.tar.gz; carry_manifest /tmp/old /tmp/new system {T1} /tmp/out; echo "rc=$?"; wc -l < /tmp/out')
        self.assertEqual(r.stdout.split(), ['openwrt-luci', 'openwrt-luci', 'rc=0', '1'], r.stdout + r.stderr)

    def test_the_slot_may_come_from_bootconfig(self):
        # the GKI kernel keeps androidboot.* in /proc/bootconfig (the bench: "Linux is not running from slot b")
        r = self.sh(f'mkrel {T2}; printf \'androidboot.slot_suffix = "_b"\\n\' > /t/bootconfig; MU5358_CMDLINE=/t/cmdline_a MU5358_BOOTCONFIG=/t/bootconfig sh $S apply >/dev/null 2>&1; echo "rc=$?"')
        self.assertIn('rc=0', r.stdout, r.stderr)
        self.assertEqual(self.installed('version'), T2)
        self.assertIn(f'KERNEL-{T2}'.encode(), (self.t / 'parts/boot_b').read_bytes())

    def test_partitions_are_found_by_their_gpt_name_without_by_name(self):
        # the bench (2026-11-02): OpenWrt has no /dev/block/by-name (Android's udev makes it); "cannot read slot a"
        r = self.sh('mkdir -p /t/sys/mmcblk0p7 /t/sys/mmcblk0p9 && echo "PARTNAME=boot_a" > /t/sys/mmcblk0p7/uevent && printf "MAJOR=179\\nPARTNAME=boot_b\\n" > /t/sys/mmcblk0p9/uevent; '
                    'MU5358_LIB=1 . $S; MU5358_BYNAME=/nonexistent MU5358_SYSBLOCK=/t/sys; BYNAME=/nonexistent SYSBLOCK=/t/sys; part_dev boot_b; part_dev boot_a; part_dev init_boot_b; echo "rc=$?"; '
                    'BYNAME=/t/parts; part_dev boot_b')
        self.assertEqual(r.stdout.split(), ['/dev/mmcblk0p9', '/dev/mmcblk0p7', 'rc=1', '/t/parts/boot_b'], r.stdout + r.stderr)

    def test_status_json(self):
        r = self.sh('sh $S status')
        self.assertEqual(r.returncode, 0, r.stderr)
        s = json.loads(r.stdout)
        self.assertEqual(s.get('ok'), 1, r.stdout[:300])
        self.assertEqual((s['ok'], s['installed'], s['layout'], s['tree'], s['tries'], s['boot'], s['boot_pending'], s['slot']),
                         (1, T1, 'tree', 'system', 2, T1, 0, 'b'))
        self.assertEqual(s['systems'][0]['name'], 'system')
        self.assertIsNone(s['previous'])
        self.assertEqual(s['lock']['held'], 0)

    def test_notice_writes_the_marker(self):
        r = self.sh(f'mkrel {T2}; sh $S notice; cat /t/run/update-available')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(f'mu5358-linux {T2} is out (this is {T1})', r.stdout)
        r = self.sh(f'mkrel {T1}; sh $S notice; ls /t/run')
        self.assertNotIn('update-available', r.stdout)

    def test_the_lock_is_held_once(self):
        r = self.sh(f'mkrel {T2}; ( flock 9; sleep 4 ) 9>>/t/lock/lock & sleep 1; sh $S apply; echo "rc=$?"')
        self.assertIn('rc=1', r.stdout)
        self.assertRegex(r.stderr, r'lock|another mu5358-update')
        self.assertEqual(self.version(), T1)

    def test_check_and_version_print_the_record(self):
        r = self.sh(f'mkrel {T2}; sh $S check; sh $S version')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(f'newest release    {T2}', r.stdout)
        self.assertIn(f'boot images: {T1}', r.stdout)
        self.assertIn('Run: mu5358-update apply', r.stdout)

    def test_bootimg_helpers_are_the_installers(self):
        block = re.compile(r'# ---- bootimg begin.*?# ---- bootimg end\n', re.S)
        a = block.search((TOP / 'installer/mu5358-install.sh').read_text(encoding='utf-8'))
        b = block.search((TOP / 'openwrt/update/mu5358-update').read_text(encoding='utf-8'))
        self.assertIsNotNone(a)
        self.assertIsNotNone(b)
        self.assertEqual(a.group(0), b.group(0))
        for fn in ('bootimg_put()', 'write_part()', 'u32()', 'be64()', 'fsize()', 'sha()'):
            self.assertIn(fn, a.group(0))
        # the block runs under Android's toybox and OpenWrt's busybox: no stat (the bench, 2026-11-02: 'stat: not found')
        self.assertNotIn('stat ', a.group(0))
        self.assertNotIn('stat -', a.group(0))

    def test_the_updater_is_under_the_hard_rules_and_has_no_slot_or_misc_code(self):
        self.assertIn(TOP / 'openwrt/update/mu5358-update', [p for p, _ in shell_scripts()])
        src = (TOP / 'openwrt/update/mu5358-update').read_text(encoding='utf-8')
        for gone in ('mu5358-next-boot', 'kernel_cmd()', 'write_boot()', 'linux_slot()', 'boot-attempts', 'mu5358_storage_disk',
                     'default-boot', 'MU5358_UBUNTU'):
            self.assertNotIn(gone, src, gone)
        self.assertIn('SYSTEMS="system"', src)
        self.assertIn('rootfs_asset() { echo mu5358-system.tar.gz; }', src)
