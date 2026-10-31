"""tools/charge/mu5358-charge.c, built for this host and run against fake sysfs trees: the decision (what input current
the charger may take, whether charging is on) and the I2C apply against a 12-byte register file in place of
/dev/i2c-6. Skipped without a C compiler (cc).

The values are the bench's of 2026-11-01: the Mac mini advertises 3 A over Type-C, Android takes 1.5 A, Linux
without this program took 500 mA. The hard cap this round is 1500 mA (docs/superpowers/specs/2026-11-01-...)."""
import os
import shutil
import signal
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from helpers import TOP

SRC = TOP / 'tools/charge/mu5358-charge.c'
PSY = 'sys/class/power_supply'
TCPM = f'{PSY}/sprd-tcpm-source-psy-sc27xx-pd'
# the master bq2560x on the bench in Linux: IINLIM 500 mA (0x04), charging on (0x9a), the rest as read
LINUX_REGS = bytes.fromhex('049ab4a6908766403410a000')
BIN = None


def setUpModule():
    global BIN
    if not shutil.which('cc'):
        raise unittest.SkipTest('no cc')
    if not SRC.is_file():
        raise AssertionError(f'no {SRC}')
    d = tempfile.mkdtemp(prefix='mu5358-charge-')
    BIN = Path(d) / 'mc'
    r = subprocess.run(['cc', '-O0', '-Wall', '-o', str(BIN), str(SRC)], capture_output=True, text=True)
    if r.returncode:
        raise AssertionError(f'build failed:\n{r.stderr}')


def mac_tree(**over):
    """The bench with the Mac mini's Type-C port, as the sysfs files read on 2026-11-01."""
    t = {f'{PSY}/usb/online': '1', f'{TCPM}/online': '1', f'{TCPM}/current_max': '3000000',
         f'{TCPM}/usb_type': '[C] PD PD_PPS', f'{PSY}/ac/online': '0', f'{PSY}/ac/type': 'USB_DCP',
         f'{PSY}/battery/temp': '274', f'{PSY}/battery/capacity': '23'}
    t.update(over)
    return t


class Base(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='mu5358-root-'))

    def tree(self, files):
        for rel, text in files.items():
            if text is None:
                continue
            p = self.root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text + '\n')

    def run_mc(self, *args, env=None, check=True):
        e = dict(os.environ, MU5358_ROOT=str(self.root))
        e.update(env or {})
        r = subprocess.run([str(BIN), *args], env=e, capture_output=True, text=True)
        if check:
            self.assertEqual(r.returncode, 0, r.stderr)
        return r

    def decide(self, files, prev=None):
        self.tree(files)
        return self.run_mc('decide', env={'MU5358_PREV': prev} if prev else None).stdout.strip()


class Decide(Base):
    def test_mac_typec_3a(self):
        self.assertEqual(self.decide(mac_tree()), 'source=typec input_ma=1500 charge=on reason=source')

    def test_sdp(self):
        t = mac_tree(**{f'{TCPM}/current_max': '500000', f'{TCPM}/usb_type': '[C]'})
        self.assertEqual(self.decide(t), 'source=sdp input_ma=500 charge=on reason=source')

    def test_dcp(self):
        t = mac_tree(**{f'{TCPM}/current_max': '500000', f'{TCPM}/usb_type': '[C]', f'{PSY}/ac/online': '1'})
        self.assertEqual(self.decide(t), 'source=dcp input_ma=1500 charge=on reason=source')

    def test_unplugged(self):
        t = mac_tree(**{f'{PSY}/usb/online': '0', f'{TCPM}/online': '0'})
        self.assertEqual(self.decide(t), 'source=none input_ma=500 charge=on reason=source')

    def test_no_tcpm_dir(self):
        t = mac_tree(**{f'{TCPM}/online': None, f'{TCPM}/current_max': None, f'{TCPM}/usb_type': None,
                        f'{PSY}/ac/online': '1', f'{PSY}/ac/type': 'USB_CDP'})
        self.assertEqual(self.decide(t), 'source=cdp input_ma=1500 charge=on reason=source')

    def test_hot(self):
        self.assertEqual(self.decide(mac_tree(**{f'{PSY}/battery/temp': '451'})),
                         'source=typec input_ma=500 charge=off reason=hot')

    def test_hot_hysteresis(self):
        self.assertEqual(self.decide(mac_tree(**{f'{PSY}/battery/temp': '420'}), prev='off:hot'),
                         'source=typec input_ma=500 charge=off reason=hot')
        self.assertEqual(self.decide(mac_tree(**{f'{PSY}/battery/temp': '399'}), prev='off:hot'),
                         'source=typec input_ma=1500 charge=on reason=source')

    def test_cold(self):
        self.assertEqual(self.decide(mac_tree(**{f'{PSY}/battery/temp': '-5'})),
                         'source=typec input_ma=500 charge=off reason=cold')

    def test_garbage_temp(self):
        self.assertEqual(self.decide(mac_tree(**{f'{PSY}/battery/temp': 'abc'})),
                         'source=typec input_ma=500 charge=on reason=notemp')
        self.assertEqual(self.decide(mac_tree(**{f'{PSY}/battery/temp': None})),
                         'source=typec input_ma=500 charge=on reason=notemp')

    def test_limit(self):
        lim = 'etc/mu5358/charge-limit'
        self.assertEqual(self.decide(mac_tree(**{lim: '80', f'{PSY}/battery/capacity': '80'})),
                         'source=typec input_ma=500 charge=off reason=limit')
        self.assertEqual(self.decide(mac_tree(**{lim: '80', f'{PSY}/battery/capacity': '78'}), prev='off:limit'),
                         'source=typec input_ma=500 charge=off reason=limit')
        self.assertEqual(self.decide(mac_tree(**{lim: '80', f'{PSY}/battery/capacity': '77'}), prev='off:limit'),
                         'source=typec input_ma=1500 charge=on reason=source')
        self.assertEqual(self.decide(mac_tree(**{lim: '100', f'{PSY}/battery/capacity': '100'})),
                         'source=typec input_ma=1500 charge=on reason=source')
        self.assertEqual(self.decide(mac_tree(**{lim: '20', f'{PSY}/battery/capacity': '50'})),
                         'source=typec input_ma=1500 charge=on reason=source')


if __name__ == '__main__':
    unittest.main()
