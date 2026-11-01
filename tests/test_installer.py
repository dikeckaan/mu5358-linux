"""installer/mu5358-install.sh's pure helpers, black-box through its `_test FUNCTION ARGS...` entry, under a Linux
busybox (what the device runs it with: Magisk's busybox) in docker. Skipped where docker or the busybox image is not
to be had (MU5358_TEST_DOCKER=0 skips on purpose).

The repository is mounted read-only at /w; what a helper writes goes to the container's own /tmp or /data."""
import hashlib
import unittest
import subprocess
import zlib

from helpers import BUSYBOX_IMAGE, DOCKER, TOP, docker_busybox

S = '/w/installer/mu5358-install.sh'
# the block the bench unit's misc had with Android on slot a (a: priority 15, 7 tries, successful; b: nothing), and
# the one-shot trial the installer makes from it (b: priority 15, 2 tries, not successful; a: priority 14, successful)
ANDROID = '5f6100004243414201020000ff000f0000000000000000000000000096b18d84'
TRIAL = '5f6200004243414201020000fe002f000000000000000000000000001c751554'
DREPPER = '$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1'
# the fixture pair of the bench unit (not in git): Android's boot_a and the rebuilt kernel give exactly the boot.img
# that slot b has had since 2026-10-29
BOOT_A, KERNEL, BOOT_B = TOP / 'work/unit2/img/boot_a.img', TOP / 'work/kernel/Image', TOP / 'work/unit2/linux-b/boot.img'


def crc_block(hex56, prefix=None):
    """A 32-byte bootloader_control block as hex: HEX56 (the first 28 bytes) with the CRC the script checks."""
    b = bytearray(bytes.fromhex(hex56))
    if prefix:
        b[0:2] = bytes.fromhex(prefix)
    return bytes(b).hex() + zlib.crc32(bytes(b)).to_bytes(4, 'little').hex()


def setUpModule():
    why = docker_busybox()
    if why:
        raise unittest.SkipTest(why)


class Busybox(unittest.TestCase):
    def sh(self, script, stdin=None, user=None, timeout=120):
        """SCRIPT under busybox sh in the container, the repository at /w (read-only)."""
        argv = [DOCKER, 'run', '--rm', '-v', f'{TOP}:/w:ro']
        if stdin is not None:
            argv.append('-i')
        if user:
            argv += ['--user', user]
        argv += [BUSYBOX_IMAGE, 'sh', '-c', script]
        return subprocess.run(argv, input=stdin, capture_output=True, text=True, timeout=timeout)

    def fn(self, name, *args, **kw):
        """The helper NAME with ARGS through the script's _test entry."""
        return self.sh(f'sh {S} _test {name} ' + ' '.join(f"'{a}'" for a in args), **kw)


class Misc(Busybox):
    def test_bc_trial_of_the_known_android_block(self):
        r = self.fn('bc_trial', ANDROID)
        self.assertEqual(r.stdout.strip(), TRIAL, r.stderr)

    def test_the_trial_is_one_shot_and_keeps_android_successful(self):
        t = TRIAL
        self.assertEqual(t[:4], '5f62', 'the current slot: b')
        self.assertEqual(t[8:16], '42434142', 'magic BCAB')
        a, b = int(t[24:26], 16), int(t[28:30], 16)
        self.assertEqual(b, 0x2f, 'b: priority 15, 2 tries, not successful')
        self.assertEqual(a & 0x80, 0x80, "a keeps its successful bit")
        self.assertEqual(a & 0x0f, 14, 'a: priority 14, below b for this one boot')
        self.assertEqual(t[32:56], ANDROID[32:56], 'nothing else changes')
        # the CRC is the script's own
        self.assertEqual(crc_block(t[:56]), t)
        self.assertEqual(crc_block(ANDROID[:56]), ANDROID)

    def test_bc_state_classifies(self):
        cases = [
            (ANDROID, 'android'),
            (TRIAL, 'trial'),
            (crc_block(ANDROID[:56], '5f63'), 'other'),            # a slot that is neither
            (crc_block(ANDROID[:24] + '0f' + ANDROID[26:56]), 'other'),   # a on slot a but not successful
            (crc_block(ANDROID[:28] + '7f' + ANDROID[30:56], '5f62'), 'other'),   # b without the one-shot byte
            (ANDROID[:-8] + '00000000', 'invalid'),                 # a wrong CRC
            (ANDROID[:-2], 'invalid'),                               # short
            ('', 'invalid'),
            (crc_block(ANDROID[:8] + '00000000' + ANDROID[16:56]), 'invalid'),   # no magic
        ]
        script = '; '.join(f"sh {S} _test bc_state '{h}'" for h, _ in cases)
        r = self.sh(script)
        self.assertEqual(r.stdout.split('\n')[:-1], [want for _, want in cases], r.stderr)

    def test_bc_valid(self):
        r = self.sh(f"sh {S} _test bc_valid '{ANDROID}'; echo $?; sh {S} _test bc_valid '{ANDROID[:-8]}00000000'; echo $?")
        self.assertEqual(r.stdout.split(), ['0', '1'])


class Zero(Busybox):
    def test_zeros_short_and_unreadable(self):
        script = f'''
head -c 100 /dev/zero > /tmp/z; : > /tmp/e; printf '\\0\\0\\0\\1' > /tmp/nz
sh {S} _test zero /tmp/z; echo "all=$?"
sh {S} _test zero /tmp/z 50; echo "part=$?"
sh {S} _test zero /tmp/z 200; echo "short=$?"
sh {S} _test zero /tmp/e; echo "empty=$?"
sh {S} _test zero /tmp/nz; echo "nonzero=$?"
sh {S} _test zero /tmp/nz 3; echo "nonzero-head=$?"
sh {S} _test zero /tmp/missing 2>/dev/null; echo "missing=$?"
sh {S} _test zero /tmp/z 0; echo "none=$?"
sh {S} _test zero /tmp/z abc 2>/dev/null; echo "bad=$?"
'''
        r = self.sh(script)
        got = dict(l.split('=') for l in r.stdout.split() if '=' in l)
        self.assertEqual(got, {'all': '0', 'part': '0', 'short': '1', 'empty': '1', 'nonzero': '1', 'nonzero-head': '0',
                               'missing': '1', 'none': '1', 'bad': '1'}, r.stdout + r.stderr)


class ZeroSampled(Busybox):
    """zero_sampled FILE: zeros at the head and at three points inside (what status asks of fulldumpdb: the full read
    of 2 GiB took the web page's 60 s and more, 2026-11-01). Data between the probes is not seen: the install path
    keeps the full check."""
    def test_probes_head_quarters_and_tail(self):
        script = f'''
head -c 16777216 /dev/zero > /tmp/z
cp /tmp/z /tmp/mid; printf 'x' | dd of=/tmp/mid bs=1 seek=6000000 conv=notrunc 2>/dev/null
cp /tmp/z /tmp/q; printf 'x' | dd of=/tmp/q bs=1 seek=4194304 conv=notrunc 2>/dev/null
cp /tmp/z /tmp/h; printf 'x' | dd of=/tmp/h bs=1 seek=8388608 conv=notrunc 2>/dev/null
cp /tmp/z /tmp/t; printf 'x' | dd of=/tmp/t bs=1 seek=16777215 conv=notrunc 2>/dev/null
cp /tmp/z /tmp/s; printf 'x' | dd of=/tmp/s bs=1 seek=0 conv=notrunc 2>/dev/null
sh {S} _test zero_sampled /tmp/z; echo "all=$?"
sh {S} _test zero_sampled /tmp/mid; echo "between=$?"
sh {S} _test zero_sampled /tmp/q; echo "quarter=$?"
sh {S} _test zero_sampled /tmp/h; echo "half=$?"
sh {S} _test zero_sampled /tmp/t; echo "tail=$?"
sh {S} _test zero_sampled /tmp/s; echo "start=$?"
sh {S} _test zero_sampled /tmp/missing 2>/dev/null; echo "missing=$?"
'''
        r = self.sh(script)
        got = dict(l.split('=') for l in r.stdout.split() if '=' in l)
        self.assertEqual(got, {'all': '0', 'between': '0', 'quarter': '1', 'half': '1', 'tail': '1', 'start': '1',
                               'missing': '1'}, r.stdout + r.stderr)


class Password(Busybox):
    def test_a_valid_hash_is_kept_for_root_only(self):
        r = self.sh(f'sh {S} password && stat -c "%a %s" /data/adb/mu5358/in/shadow-hash && cat /data/adb/mu5358/in/shadow-hash'
                    f' && stat -c %a /data/adb/mu5358 /data/adb/mu5358/in', stdin=DREPPER + '\n')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout.splitlines(), ['MU5358-PASSWORD set', f'600 {len(DREPPER) + 1}', DREPPER, '700', '700'])

    def test_anything_but_a_sha512_crypt_hash_is_refused(self):
        bad = ['x', 'password', '', DREPPER[:-1], DREPPER + 'a', '$5$' + DREPPER[3:], '$6$$' + DREPPER.split('$')[3],
               '$6$' + 'a' * 17 + '$' + DREPPER.split('$')[3], DREPPER.replace('/', '!'), DREPPER + ' x',
               DREPPER.replace('$', '$ ', 1)]
        for h in bad:
            with self.subTest(hash=h):
                r = self.sh(f'sh {S} password; echo "rc=$?"; ls /data/adb/mu5358/in/shadow-hash 2>&1', stdin=h + '\n')
                self.assertIn('MU5358-ERROR not a sha512-crypt hash', r.stdout)
                self.assertIn('rc=1', r.stdout)
                self.assertIn('No such file', r.stdout)

    def test_blanks_around_the_hash_are_trimmed(self):
        # `read` drops the blanks at either end: the hash is kept as the regex takes it
        for h in (' ' + DREPPER, DREPPER + ' ', '\t' + DREPPER + ' \t'):
            with self.subTest(hash=h):
                r = self.sh(f'sh {S} password && cat /data/adb/mu5358/in/shadow-hash', stdin=h + '\n')
                self.assertEqual(r.stdout.splitlines(), ['MU5358-PASSWORD set', DREPPER])

    def test_a_hash_is_one_line(self):
        # only the first line is read: a second line never reaches the file
        r = self.sh(f'sh {S} password; cat /data/adb/mu5358/in/shadow-hash', stdin=DREPPER + '\nsecond line\n')
        self.assertEqual(r.stdout.splitlines(), ['MU5358-PASSWORD set', DREPPER])


class BootImage(Busybox):
    def test_refuses_what_is_not_a_v4_boot_image(self):
        r = self.fn('bootimg_put', '/w/README.md', '/w/LICENSE', 'kernel', '/tmp/out.img')
        self.assertEqual(r.returncode, 1)
        self.assertIn('not a v4 boot image', r.stdout)

    @unittest.skipUnless(BOOT_A.is_file() and KERNEL.is_file() and BOOT_B.is_file(),
                         'the bench unit\'s fixtures (work/unit2/img/boot_a.img, work/kernel/Image, work/unit2/linux-b/boot.img) are not here')
    def test_the_kernel_in_androids_boot_image_is_the_installed_boot_b(self):
        want = hashlib.sha256(BOOT_B.read_bytes()).hexdigest()
        r = self.sh(f'sh {S} _test bootimg_put /w/{BOOT_A.relative_to(TOP)} /w/{KERNEL.relative_to(TOP)} kernel /tmp/out.img'
                    f' && sha256sum /tmp/out.img', timeout=600)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout.split()[-2], want, 'the image differs from work/unit2/linux-b/boot.img')

    @unittest.skipUnless(BOOT_A.is_file(), 'the bench unit\'s boot_a.img is not here')
    def test_a_payload_too_large_for_the_partition_is_refused(self):
        a = f'/w/{BOOT_A.relative_to(TOP)}'
        r = self.sh(f'sh {S} _test bootimg_put {a} {a} kernel /tmp/out.img; echo "rc=$?"; ls /tmp/out.img 2>&1', timeout=600)
        self.assertIn('too large for the partition', r.stdout)
        self.assertIn('rc=1', r.stdout)
        self.assertIn('No such file', r.stdout, 'a refused image must leave no output file')

    @unittest.skipUnless(BOOT_A.is_file(), 'the bench unit\'s boot_a.img is not here')
    def test_a_ramdisk_does_not_go_where_the_kernel_is(self):
        # boot_a holds a kernel: a ramdisk may only replace the payload of an image that has a ramdisk and no kernel
        a = f'/w/{BOOT_A.relative_to(TOP)}'
        r = self.sh(f'sh {S} _test bootimg_put {a} /w/LICENSE ramdisk /tmp/out.img; echo "rc=$?"', timeout=600)
        self.assertIn('has more than a ramdisk', r.stdout)
        self.assertIn('rc=1', r.stdout)


class Jobs(Busybox):
    def test_a_pid_that_is_not_the_installer_is_no_running_job(self):
        # the pid file outlives a reboot; its number can be another process's by then (the bench, 2026-11-01)
        script = f'''
mkdir -p /data/adb/mu5358/job
(exec sleep 30) & echo $! > /data/adb/mu5358/job/pid
sh {S} _test job_running; echo "other=$?"
rm /data/adb/mu5358/job/pid
sh {S} _test job_running; echo "none=$?"
echo 999999 > /data/adb/mu5358/job/pid
sh {S} _test job_running; echo "dead=$?"
'''
        r = self.sh(script)
        self.assertEqual(r.stdout.split(), ['other=1', 'none=1', 'dead=1'], r.stderr)


class Entry(Busybox):
    def test_not_root_is_refused_before_anything(self):
        r = self.sh(f'sh {S} status', user='1000:1000')
        self.assertEqual(r.returncode, 2)
        self.assertEqual(r.stdout.strip(), 'MU5358-ERROR not-root')

    def test_no_command_prints_the_usage(self):
        r = self.sh(f'sh {S}')
        self.assertEqual(r.returncode, 2)
        self.assertIn('status', r.stdout)
        self.assertIn('boot-linux', r.stdout)
        self.assertNotIn('set -u', r.stdout)

    def test_an_unknown_command_or_file_is_refused(self):
        r = self.sh(f'sh {S} cat ../../etc/passwd; echo "rc=$?"; sh {S} run format; echo "rc=$?"; sh {S} _install; echo "rc=$?"')
        self.assertEqual(r.stdout.count('MU5358-ERROR'), 3, r.stdout)
        self.assertEqual(r.stdout.count('rc=1'), 3)
        self.assertIn('MU5358-ERROR no such file', r.stdout)
        self.assertRegex(r.stdout, r'MU5358-ERROR run install\|(\w+\|)*uninstall')
        # _install without a release taken: check dies before anything is written (it is not a U30 Pro either)
        self.assertRegex(r.stdout, r'MU5358-STEP check\nMU5358-ERROR not a ZTE U30 Pro')


if __name__ == '__main__':
    unittest.main()
