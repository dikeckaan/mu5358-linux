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

    def test_pd_contract_below_1500(self):
        """A PD contract is honoured as negotiated: 5 V/1 A gives 1000 mA, never the cap (a small adapter would collapse)."""
        t = mac_tree(**{f'{TCPM}/current_max': '1000000', f'{TCPM}/usb_type': 'C [PD] PD_PPS'})
        self.assertEqual(self.decide(t), 'source=typec input_ma=1000 charge=on reason=source')
        t = mac_tree(**{f'{TCPM}/current_max': '3000000', f'{TCPM}/usb_type': 'C [PD] PD_PPS'})
        self.assertEqual(self.decide(t), 'source=typec input_ma=1500 charge=on reason=source')
        t = mac_tree(**{f'{TCPM}/current_max': '300000', f'{TCPM}/usb_type': 'C [PD] PD_PPS'})
        self.assertEqual(self.decide(t), 'source=typec input_ma=500 charge=on reason=source')

    def test_limit_holds_without_a_temperature(self):
        lim = 'etc/mu5358/charge-limit'
        self.assertEqual(self.decide(mac_tree(**{f'{PSY}/battery/temp': 'abc', lim: '80', f'{PSY}/battery/capacity': '80'})),
                         'source=typec input_ma=500 charge=off reason=limit')
        self.assertEqual(self.decide(mac_tree(**{f'{PSY}/battery/temp': 'abc', lim: '80', f'{PSY}/battery/capacity': '50'})),
                         'source=typec input_ma=500 charge=on reason=notemp')

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



class Apply(Base):
    """The register image stands in for /dev/i2c-6 (MU5358_CHARGE_I2C names a regular file): REG00 and REG01 are
    the only bytes that may change."""

    def setUp(self):
        super().setUp()
        self.regs = self.root / 'regs'
        self.regs.write_bytes(LINUX_REGS)
        self.state = self.root / 'state'
        self.env = {'MU5358_CHARGE_I2C': str(self.regs), 'MU5358_STATE': str(self.state)}

    def once(self, files, **env):
        self.tree(files)
        return self.run_mc('once', env=dict(self.env, **env)).stdout.strip()

    def fields(self, line):
        return dict(kv.split('=', 1) for kv in line.split())

    def test_once_mac(self):
        f = self.fields(self.once(mac_tree()))
        self.assertEqual((f['input_ma'], f['charge'], f['reg00'], f['reg01'], f['rewrites']), ('1500', 'on', '0x0e', '0x9a', '0'))
        self.assertEqual(self.regs.read_bytes()[0], 0x0e)
        self.assertEqual(self.state.read_text().strip(), self.once(mac_tree()))  # the state file is the line

    def test_hot_turns_charging_off(self):
        self.once(mac_tree(**{f'{PSY}/battery/temp': '460'}))
        b = self.regs.read_bytes()
        self.assertEqual((b[0], b[1]), (0x04, 0x8a))

    def test_rewrite_counted(self):
        self.assertEqual(self.fields(self.once(mac_tree()))['rewrites'], '0')
        self.regs.write_bytes(bytes([0x04]) + self.regs.read_bytes()[1:])  # the vendor driver put 500 mA back
        self.assertEqual(self.fields(self.once(mac_tree()))['rewrites'], '1')
        self.assertEqual(self.regs.read_bytes()[0], 0x0e)

    def test_never_touches_other_bytes(self):
        self.once(mac_tree(**{f'{PSY}/battery/temp': '460'}))
        self.once(mac_tree())
        self.assertEqual(self.regs.read_bytes()[2:], LINUX_REGS[2:])

    def test_sigterm_restores(self):
        self.tree(mac_tree())
        e = dict(os.environ, MU5358_ROOT=str(self.root), MU5358_CHARGE_PERIOD='1', **self.env)
        p = subprocess.Popen([str(BIN)], env=e, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        for _ in range(50):
            if self.state.exists():
                break
            time.sleep(0.1)
        self.assertTrue(self.state.exists(), 'no state file after 5 s')
        self.assertEqual(self.regs.read_bytes()[0], 0x0e)
        p.send_signal(signal.SIGTERM)
        self.assertEqual(p.wait(timeout=5), 0, p.stderr.read())
        b = self.regs.read_bytes()
        self.assertEqual(b[0], 0x04)
        self.assertTrue(b[1] & 0x10)
        self.assertFalse(self.state.exists())

    def test_state_directory_is_made(self):
        state = self.root / 'run/mu5358/charge'
        self.once(mac_tree(), MU5358_STATE=str(state))
        self.assertTrue(state.is_file())

    def test_loop_survives_an_apply_failure(self):
        """No state file, the attribute unreadable from the first round: the loop keeps going (the next decision
        must not read an unset previous one), says i2c-error, and SIGTERM still exits 0."""
        bad = self.root / 'registers'
        bad.write_text('nothing a charger would say\n')
        self.tree(mac_tree())
        e = dict(os.environ, MU5358_ROOT=str(self.root), MU5358_CHARGE_PERIOD='1', MU5358_CHARGE_REGS=str(bad),
                 MU5358_STATE=str(self.state))
        p = subprocess.Popen([str(BIN)], env=e, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(2.5)
        self.assertIsNone(p.poll(), 'the daemon died')
        self.assertIn('reason=i2c-error', self.state.read_text())
        p.send_signal(signal.SIGTERM)
        self.assertEqual(p.wait(timeout=5), 0)

    def test_status_prints_state(self):
        line = self.once(mac_tree())
        self.assertEqual(self.run_mc('status', env=self.env).stdout.strip(), line)

    def test_limit_cli(self):
        lim = self.root / 'etc/mu5358/charge-limit'
        self.run_mc('limit', '80')
        self.assertEqual(lim.read_text(), '80\n')
        self.assertEqual(self.run_mc('limit', '20', check=False).returncode, 2)
        self.assertEqual(lim.read_text(), '80\n')
        self.run_mc('limit', 'off')
        self.assertEqual(lim.read_text(), '100\n')
        self.assertEqual(self.run_mc('limit').stdout.strip(), '100')



REGS_TEXT = """Master Reg:
Reg[00] = 0x04
Reg[01] = 0x9a
Reg[02] = 0xb4
Reg[03] = 0xa6
Reg[04] = 0x90
Reg[05] = 0x87
Reg[06] = 0x66
Reg[07] = 0x40
Reg[08] = 0x34
Reg[09] = 0x10
Reg[0a] = 0xa0
Reg[0b] = 0x00
SLAVE Reg:
Reg[00] = 0x84
Reg[01] = 0x8a
Reg[02] = 0xb4
Reg[03] = 0xa6
Reg[04] = 0x90
Reg[05] = 0x07
Reg[06] = 0x66
Reg[07] = 0x40
Reg[08] = 0x24
Reg[09] = 0x00
Reg[0a] = 0x80
Reg[0b] = 0x00
"""


class ApplySysfs(Base):
    """The vendor driver's registers attribute (/sys/bus/i2c/devices/6-006b/registers: the Master and SLAVE blocks on
    read; "0 REG VAL" on write for the master) is the first way to the chip; i2c-dev the fallback. A regular file
    named in MU5358_CHARGE_REGS stands in for the attribute: a write of "0 REG VAL" patches its Master block."""

    def setUp(self):
        super().setUp()
        self.regs = self.root / 'registers'
        self.regs.write_text(REGS_TEXT)
        self.i2c = self.root / 'i2c'
        self.i2c.write_bytes(LINUX_REGS)
        self.state = self.root / 'state'
        self.env = {'MU5358_CHARGE_REGS': str(self.regs), 'MU5358_CHARGE_I2C': str(self.i2c), 'MU5358_STATE': str(self.state)}

    def once(self, files, **env):
        self.tree(files)
        return self.run_mc('once', env=dict(self.env, **env)).stdout.strip()

    def master(self, reg):
        block = self.regs.read_text().split('SLAVE Reg:')[0]
        return int(block.split(f'Reg[{reg:02x}] = ')[1][:4], 16)

    def test_once_mac_through_the_attribute(self):
        line = self.once(mac_tree())
        self.assertIn('input_ma=1500 charge=on', line)
        self.assertIn('reg00=0x0e reg01=0x9a', line)
        self.assertEqual(self.master(0), 0x0e)
        self.assertEqual(self.i2c.read_bytes(), LINUX_REGS, 'i2c-dev untouched while the attribute is there')
        self.assertIn('SLAVE Reg:\nReg[00] = 0x84', self.regs.read_text(), 'the slave block untouched')

    def test_hot_off_through_the_attribute(self):
        self.once(mac_tree(**{f'{PSY}/battery/temp': '460'}))
        self.assertEqual((self.master(0), self.master(1)), (0x04, 0x8a))

    def test_sigterm_restores_through_the_attribute(self):
        self.tree(mac_tree())
        e = dict(os.environ, MU5358_ROOT=str(self.root), MU5358_CHARGE_PERIOD='1', **self.env)
        p = subprocess.Popen([str(BIN)], env=e, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        for _ in range(50):
            if self.state.exists():
                break
            time.sleep(0.1)
        self.assertEqual(self.master(0), 0x0e)
        p.send_signal(signal.SIGTERM)
        self.assertEqual(p.wait(timeout=5), 0, p.stderr.read())
        self.assertEqual(self.master(0), 0x04)
        self.assertTrue(self.master(1) & 0x10)

    def test_fallback_to_i2c_dev_without_the_attribute(self):
        self.regs.unlink()
        line = self.once(mac_tree())
        self.assertIn('reg00=0x0e', line)
        self.assertEqual(self.i2c.read_bytes()[0], 0x0e)

if __name__ == '__main__':
    unittest.main()
