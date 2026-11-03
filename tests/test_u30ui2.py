"""u30ui2, the dashboard, driven on the host (tools/u30ui2/sim.c: the real ui.c with a virtual clock, stubs for the
display, the touch panel and the data thread, a scripted event language on stdin, PNG screenshots). Built once per run
in the build image; every test is one run of the simulator. docs/superpowers/specs/2026-11-03-kano-safe-package-design.md."""
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

from helpers import DOCKER, TOP, docker_image_ready

IMAGE = os.environ.get('MU5358_TEST_BUILD_IMAGE', 'mu5358-mainline-build')
PAGES = ('home', 'net', 'wifi', 'clients', 'traffic', 'sms', 'battery', 'system', 'settings', 'power', 'log')
LANGS = ('en', 'tr', 'zh')


def png_pixels(path):
    """[(r, g, b), ...] of an RGB PNG (the writer in tools/u30ui/png.c: one IDAT, filter 0 on every row)."""
    b = Path(path).read_bytes()
    assert b[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat, w, h = 8, b'', 0, 0
    while pos < len(b):
        n, kind = struct.unpack('>I4s', b[pos:pos + 8])
        data = b[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h = struct.unpack('>II', data[:8])
        elif kind == b'IDAT':
            idat += data
        pos += 12 + n
    raw = zlib.decompress(idat)
    row = 1 + 3 * w
    out = []
    for y in range(h):
        line = raw[y * row:(y + 1) * row]
        assert line[0] == 0, 'filter 0'
        out.extend((line[1 + 3 * x], line[2 + 3 * x], line[3 + 3 * x]) for x in range(w))
    return out


def nonblank_share(path):
    px = png_pixels(path)
    drawn = sum(1 for p in px if p not in ((0, 0, 0), (0x40, 0x40, 0x40)))
    return drawn / len(px)


class Sim(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        why = docker_image_ready(IMAGE)
        if why:
            raise unittest.SkipTest(why)
        cls.build = Path(tempfile.mkdtemp(prefix='mu5358-u30ui2-sim-'))
        r = subprocess.run([DOCKER, 'run', '--rm', '-v', f'{TOP}/tools:/t:ro', '-v', f'{cls.build}:/b', IMAGE, 'bash',
                            '/t/u30ui2/build.sh', 'sim', '/b/sim'], capture_output=True, text=True, timeout=900)
        if r.returncode != 0:
            shutil.rmtree(cls.build, ignore_errors=True)
            raise AssertionError('the simulator does not build: ' + (r.stdout + r.stderr)[-3000:])

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.build, ignore_errors=True)

    def setUp(self):
        self.t = Path(tempfile.mkdtemp(prefix='mu5358-u30ui2-case-'))
        self.addCleanup(shutil.rmtree, self.t, ignore_errors=True)

    def run_sim(self, script, want_rc=0):
        r = subprocess.run([DOCKER, 'run', '--rm', '-i', '-v', f'{self.build}:/b:ro', '-v', f'{self.t}:/o', IMAGE, '/b/sim', '/o'],
                           capture_output=True, text=True, input=script, timeout=300)
        self.assertEqual(r.returncode, want_rc, r.stdout[-2000:] + r.stderr[-2000:])
        self.assertNotIn('Sanitizer', r.stderr)
        return r.stdout

    def states(self, out):
        return [dict(kv.split('=', 1) for kv in line.split()[1:]) for line in out.splitlines() if line.startswith('STATE ')]

    def runs(self, out):
        return [line[4:] for line in out.splitlines() if line.startswith('RUN ')]

    def test_every_page_every_language_renders(self):
        script = 'set lock off\n' + ''.join(f'lang {l}\n' + ''.join(f'page {p}\nshot {p}-{l}\n' for p in PAGES) for l in LANGS)
        self.run_sim(script)
        for l in LANGS:
            for p in PAGES:
                share = nonblank_share(self.t / f'{p}-{l}.png')
                self.assertGreater(share, 0.02, f'{p}-{l}: {share:.3f} of the pixels drawn')

    def test_swipe_moves_and_settles(self):
        out = self.run_sim('set lock off\nswipe 200 120 60 120 240\nwait 400\nstate\nswipe 60 120 200 120 240\nwait 400\nstate\n')
        self.assertEqual([s['page'] for s in self.states(out)], ['net', 'home'])
        self.assertEqual(self.states(out)[0]['settle'], '0', 'the animation is over after 400 ms')

    def test_swipe_down_goes_home(self):
        out = self.run_sim('set lock off\npage wifi\nswipe 120 40 120 180 240\nwait 400\nstate\n')
        self.assertEqual(self.states(out)[0]['page'], 'home')

    def test_settings_rows_cycle(self):
        out = self.run_sim('set lock off\npage settings\ntap 120 72\nstate\ntap 120 96\nstate\ntap 120 192\nstate\n')
        s = self.states(out)
        self.assertEqual(s[0]['lang'], 'tr')
        self.assertEqual(s[1]['timeout'], '300')
        self.assertEqual(self.runs(out), ['/opt/mu5358/bin/mu5358-usb storage on'])

    def test_power_hold_runs_action(self):
        out = self.run_sim('set lock off\npage power\ndown 120 72\nwait 1100\nup\ndown 120 72\nwait 500\nup\nwait 100\n')
        self.assertEqual(self.runs(out), ['sync; reboot'])

    def test_idle_timeout_turns_off_and_locks(self):
        out = self.run_sim('set timeout 30\nhold 120 120 700\nstate\nwait 31000\nstate\n')
        s = self.states(out)
        self.assertEqual((s[0]['screen'], s[0]['locked']), ('on', '0'), 'the long press unlocked')
        self.assertEqual((s[1]['screen'], s[1]['locked']), ('off', '1'))

    def test_nfc_row_toggles(self):
        out = self.run_sim('set lock off\npage wifi\nset nfc 1\ntap 120 142\nset nfc 2\ntap 120 142\nwait 100\n')
        self.assertEqual(self.runs(out), ['/opt/mu5358/bin/mu5358-nfc-sync toggle'])

    def test_unknown_command_fails(self):
        self.run_sim('page home\nbogus 1 2\n', want_rc=2)


if __name__ == '__main__':
    unittest.main()
