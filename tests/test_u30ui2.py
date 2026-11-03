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
        # k=v pairs; the last one (nfc=) may hold spaces
        return [dict(kv.split('=', 1) for kv in line[6:].split(' ', 8)) for line in out.splitlines() if line.startswith('STATE ')]

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
        out = self.run_sim('set lock off\npage wifi\nset nfc 1\ntap 120 131\ntap 120 142\nset nfc 2\ntap 120 142\nwait 100\n')
        self.assertEqual(self.runs(out), ['/opt/mu5358/bin/mu5358-nfc-sync toggle'], 'the clients line (y 131) is not the row')

    def test_nfc_row_shows_the_result(self):
        out = self.run_sim('page wifi\nset nfc 1\nset nfc_result wpa2\nstate\nset nfc_result error\nstate\nset nfc_result no_tag\nstate\n'
                           'set nfc_result unsupported\nstate\nset nfc 0\nset nfc_result disabled\nstate\nset nfc 2\nstate\n')
        self.assertEqual([s['nfc'] for s in self.states(out)], ['On', 'NFC failed', 'NFC failed', 'Unsupported', 'Off', 'Unsupported'])

    def test_unknown_command_fails(self):
        self.run_sim('page home\nbogus 1 2\n', want_rc=2)

    # --- event wake: the charger's uevent turns the screen on; nothing else does, nothing polls
    def test_uevent_ac_wakes(self):
        out = self.run_sim('screen off\nwait 100\nstate\nuevent ac 1\nwait 100\nstate\n')
        s = self.states(out)
        self.assertEqual((s[0]['screen'], s[0]['locked']), ('off', '1'))
        self.assertEqual((s[1]['screen'], s[1]['locked']), ('on', '1'), 'the screen wakes, the lock stays')

    def test_uevent_same_value_no_wake(self):
        out = self.run_sim('uevent ac 1\nscreen off\nwait 100\nuevent ac 1\nwait 100\nstate\nuevent ac 0\nwait 100\nstate\n')
        s = self.states(out)
        self.assertEqual(s[0]['screen'], 'off', 'the same online value again is not an edge')
        self.assertEqual(s[1]['screen'], 'on', 'the unplug is')

    def test_battery_flood_ignored(self):
        out = self.run_sim('screen off\nwait 100\n' + 'uevent battery 1\nuevent battery 0\n' * 500 + 'wait 100\nstate\n')
        self.assertEqual(self.states(out)[0]['screen'], 'off')

    def test_flood_keeps_input(self):
        out = self.run_sim('set lock off\npage settings\n' + 'uevent battery 1\nuevent battery 0\n' * 500 + 'tap 120 72\nstate\n')
        self.assertEqual(self.states(out)[0]['lang'], 'tr')

    # --- the power key: "power" opens the Power page through the lock, its hold ring still guards the actions
    def test_power_cmd_wakes_locked(self):
        out = self.run_sim('set lock hold\nscreen off\nwait 100\ncmd power\nwait 100\nstate\n'
                           'swipe 200 120 60 120 240\nwait 400\nstate\ndown 120 72\nwait 1100\nup\nwait 100\nstate\n')
        s = self.states(out)
        self.assertEqual((s[0]['page'], s[0]['screen'], s[0]['locked']), ('power', 'on', '1'))
        self.assertEqual(s[1]['page'], 'power', 'a swipe does not leave the page while locked')
        self.assertEqual(self.runs(out), ['sync; reboot'], 'the hold ring runs the action')

    def test_key_long_is_the_power_cmd(self):
        out = self.run_sim('screen off\nkey 116 long\nwait 100\nstate\nkey 116 short\nstate\n')
        s = self.states(out)
        self.assertEqual((s[0]['page'], s[0]['screen']), ('power', 'on'))
        self.assertEqual(s[1]['screen'], 'off')


class Fuzz(Sim):
    """4000 random events against the ASan/UBSan build: no crash, no sanitizer report, an empty stderr."""

    @classmethod
    def setUpClass(cls):
        why = docker_image_ready(IMAGE)
        if why:
            raise unittest.SkipTest(why)
        cls.build = Path(tempfile.mkdtemp(prefix='mu5358-u30ui2-asan-'))
        r = subprocess.run([DOCKER, 'run', '--rm', '-v', f'{TOP}/tools:/t:ro', '-v', f'{cls.build}:/b', IMAGE, 'bash',
                            '/t/u30ui2/build.sh', 'sim', '/b/sim', 'asan'], capture_output=True, text=True, timeout=900)
        if r.returncode != 0:
            shutil.rmtree(cls.build, ignore_errors=True)
            raise AssertionError('the ASan simulator does not build: ' + (r.stdout + r.stderr)[-3000:])

    @staticmethod
    def script(seed, n=4000):
        import random
        r = random.Random(seed)
        out = ['set lock off']
        for i in range(n):
            k = r.random()
            x, y = r.randint(0, 239), r.randint(0, 239)
            if k < .30: out.append('tap %d %d' % (x, y))
            elif k < .50: out.append('swipe %d %d %d %d %d' % (x, y, r.randint(0, 239), r.randint(0, 239), r.choice([80, 160, 400])))
            elif k < .58: out.append('hold %d %d %d' % (x, y, r.choice([300, 900, 1500, 2500])))
            elif k < .64: out.append('key 116 %s' % r.choice(['short', 'long']))
            elif k < .72: out.append('wait %d' % r.choice([50, 500, 5000, 70000, 200000]))
            elif k < .78: out.append('page %s' % r.choice(PAGES))
            elif k < .82: out.append('lang %s' % r.choice(LANGS))
            elif k < .88: out.append('set %s %d' % (r.choice(['nfc', 'usb_storage', 'battery', 'charging', 'wifi', 'signal', 'nsms']), r.randint(0, 100)))
            elif k < .91: out.append('uevent %s %d' % (r.choice(['ac', 'usb', 'battery']), r.randint(0, 1)))
            elif k < .93: out.append('screen %s' % r.choice(['on', 'off']))
            elif k < .95: out.append('down %d %d' % (x, y))
            elif k < .97: out.append('move %d %d' % (x, y))
            elif k < .99: out.append('up')
            else: out.append('shot fuzz%d' % (i % 3))
        return '\n'.join(out) + '\nstate\n'

    def test_seeded_fuzz(self):
        seeds = [1] + ([int(os.environ['MU5358_FUZZ_SEED'])] if os.environ.get('MU5358_FUZZ_SEED') else [])
        for seed in seeds:
            with self.subTest(seed=seed):
                r = subprocess.run([DOCKER, 'run', '--rm', '-i', '-e', 'ASAN_OPTIONS=detect_leaks=0', '-v', f'{self.build}:/b:ro',
                                    '-v', f'{self.t}:/o', IMAGE, '/b/sim', '/o'], capture_output=True, text=True,
                                   input=self.script(seed), timeout=600)
                self.assertEqual(r.returncode, 0, r.stderr[-3000:])
                self.assertEqual(r.stderr, '')
                self.assertIn('STATE ', r.stdout)

    # the inherited scenarios run on the ASan build too


if __name__ == '__main__':
    unittest.main()
