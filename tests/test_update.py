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


if __name__ == '__main__':
    unittest.main()
