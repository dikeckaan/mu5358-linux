"""boot/init's tree-select block (W2, docs/superpowers/plans/2026-11-02-updater-from-linux.md): the filesystem holds
system/ (the tree), system.old/ (the previous one), data/ and boot/{tree,tries,ok}; init picks the tree, counts the
boots without boot/ok, falls back to system.old after three, and binds data/ and the whole filesystem into the tree.
The block is cut out of boot/init between its begin/end markers and run under sh with mount stubbed."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

TOP = Path(__file__).resolve().parent.parent
INIT = TOP / 'boot/init'

HARNESS = """
log() {{ echo "LOG $*"; }}
mount() {{ echo "MOUNT $*"; }}
MU={mu}
{block}
if tree_select {disk}; then
    echo "TREE $TREE"
    tree_mount {disk} {newroot}
    ramdisk_publish {newroot} fulldumpdb
else
    echo "TREE none"
fi
"""


class TreeSelect(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='mu5358-tree-'))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.disk = self.tmp / 'disk'
        self.newroot = self.tmp / 'newroot'
        self.newroot.mkdir()
        self.mu = self.tmp / 'mu'
        (self.mu / 'modules').mkdir(parents=True)
        (self.mu / 'busybox').write_text('bb')
        (self.mu / 'e2fsck').write_text('fsck')
        (self.mu / 'misc-bc-android.bin').write_text('bc')
        (self.mu / 'modules/sprd_pmic_wdt.ko').write_text('ko')
        m = re.search(r'# --- tree-select begin\n(.*?)# --- tree-select end\n', INIT.read_text(encoding='utf-8'), re.S)
        self.assertIsNotNone(m, 'boot/init has no "# --- tree-select begin/end" block')
        self.block = m.group(1)

    def tree(self, name, version):
        d = self.disk / name
        (d / 'sbin').mkdir(parents=True)
        (d / 'sbin/init').write_text('#!/bin/sh\n')
        (d / 'etc/mu5358').mkdir(parents=True)
        (d / 'etc/mu5358/version').write_text(version + '\n')
        return d

    def boot(self, **files):
        (self.disk / 'boot').mkdir(exist_ok=True)
        for k, v in files.items():
            (self.disk / 'boot' / k).write_text(v + '\n')

    def run_block(self):
        self.disk.mkdir(exist_ok=True)
        script = HARNESS.format(mu=self.mu, block=self.block, disk=self.disk, newroot=self.newroot)
        r = subprocess.run(['sh', '-c', script], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stderr, '')
        return r.stdout

    def tries(self):
        return (self.disk / 'boot/tries').read_text().strip()

    def test_default_tree_is_system(self):
        self.tree('system', 'v2026.11.02.1')
        out = self.run_block()
        self.assertIn('TREE system\n', out)
        self.assertEqual(self.tries(), '1')
        self.assertNotIn('fallback', out)

    def test_tries_count_up_and_three_without_ok_fall_back(self):
        self.tree('system', 'v2026.11.02.2')
        self.tree('system.old', 'v2026.11.02.1')
        self.boot(tree='system', tries='2')
        out = self.run_block()
        self.assertIn('TREE system\n', out)
        self.assertEqual(self.tries(), '3')
        out = self.run_block()
        self.assertIn('TREE system.old\n', out)
        self.assertIn('LOG stage=tree-fallback tries=3', out)

    def test_ok_for_this_version_resets_nothing_but_keeps_booting_it(self):
        self.tree('system', 'v2026.11.02.2')
        self.tree('system.old', 'v2026.11.02.1')
        self.boot(tree='system', tries='7', ok='v2026.11.02.2')
        out = self.run_block()
        self.assertIn('TREE system\n', out)

    def test_missing_tree_falls_back(self):
        self.tree('system.old', 'v2026.11.02.1')
        self.boot(tree='system')
        out = self.run_block()
        self.assertIn('TREE system.old\n', out)
        self.assertIn('LOG stage=tree-fallback missing', out)

    def test_bad_name_ignored(self):
        self.tree('system', 'v2026.11.02.1')
        self.boot(tree='../etc')
        out = self.run_block()
        self.assertIn('LOG stage=tree-name-ignored', out)
        self.assertIn('TREE system\n', out)

    def test_flat_install_still_boots(self):
        self.disk.mkdir()
        (self.disk / 'sbin').mkdir()
        (self.disk / 'sbin/init').write_text('')
        out = self.run_block()
        self.assertIn('LOG stage=tree-flat', out)
        self.assertIn('TREE .\n', out)
        self.assertIn(f'MOUNT --bind {self.disk} {self.newroot}\n', out)
        self.assertIn(f'MOUNT --bind {self.disk} {self.newroot}/mnt/mu5358-disk\n', out, 'the updater can say "flat: update from the page" (review #8)')
        self.assertFalse((self.disk / 'boot/tries').exists())

    def test_nothing_to_boot(self):
        self.disk.mkdir()
        out = self.run_block()
        self.assertIn('TREE none', out)

    def test_data_and_disk_are_bind_mounted(self):
        self.tree('system', 'v2026.11.02.1')
        out = self.run_block()
        self.assertIn(f'MOUNT --bind {self.disk}/system {self.newroot}\n', out)
        self.assertIn(f'MOUNT --bind {self.disk} {self.newroot}/mnt/mu5358-disk\n', out)
        self.assertIn(f'MOUNT --bind {self.disk}/data {self.newroot}/data\n', out)
        self.assertTrue((self.disk / 'data').is_dir())

    def test_ramdisk_files_published(self):
        self.tree('system', 'v2026.11.02.1')
        self.run_block()
        rd = self.newroot / 'run/mu5358/ramdisk'
        self.assertEqual((rd / 'busybox').read_text(), 'bb')
        self.assertEqual((rd / 'misc-bc-android.bin').read_text(), 'bc')
        self.assertEqual((rd / 'modules/sprd_pmic_wdt.ko').read_text(), 'ko')
        self.assertFalse((rd / 'e2fsck').exists(), 'e2fsck is not the updater\'s')
        self.assertEqual((self.newroot / 'run/mu5358/root-target').read_text().strip(), 'fulldumpdb', 'the updater keeps the record\'s target (review #2)')


if __name__ == '__main__':
    unittest.main()
