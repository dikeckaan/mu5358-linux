"""usb-watch (openwrt/usb/usb-watch): the gadget re-enumerated while no host has it, patiently. Runs the real script
under the host's sh against a fake gadget, UDC state, /proc/net/dev and uptime, with stubs for ip, logger and sleep on
PATH; the sleep stub advances the fake uptime instead of waiting and ends the script after a budget of fake seconds.
A re-enumeration is a write of '' then the UDC name to the gadget's UDC file (the stub log keeps every write via a
sentinel: the UDC file's history)."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

TOP = Path(__file__).resolve().parent.parent
SCRIPT = TOP / 'openwrt/usb/usb-watch'
UDC = 'musb-hdrc.1.auto'

SLEEP = '''#!/bin/sh
# fake time: add the seconds to the uptime file; stop the watch after the budget
t=$(cut -d. -f1 "$MU5358_UPTIME"); t=$((t + ${1:-0}))
echo "$t.00 0.00" > "$MU5358_UPTIME"
[ "$t" -lt "$BUDGET" ] || kill -TERM $PPID
exit 0
'''
IP = '''#!/bin/sh
# the link flags from a file the test writes (LOWER_UP or nothing)
cat "$FAKE_LINK" 2>/dev/null
'''
LOGGER = '''#!/bin/sh
echo "$*" >> "$STUBLOG"
'''


class Watch(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='mu5358-watch-'))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.g = self.tmp / 'g'
        self.g.mkdir()
        (self.g / 'UDC').write_text(UDC + '\n')
        self.udc = self.tmp / 'udc' / UDC
        self.udc.mkdir(parents=True)
        (self.udc / 'state').write_text('configured\n')
        self.netdev = self.tmp / 'netdev'
        self.netdev.write_text('Inter-|   Receive\n face |bytes packets\n'
                               '  usb0: 100 10 0 0 0 0 0 0 200 20 0 0 0 0 0 0\n')
        self.uptime = self.tmp / 'uptime'
        self.uptime.write_text('1000.00 0.00\n')
        self.usbtype = self.tmp / 'usb_type'
        self.usbtype.write_text('[SDP] CDP DCP\n')
        self.stamp = self.tmp / 'run' / 'usb-rebound'
        self.link = self.tmp / 'link'
        self.log = self.tmp / 'log'
        self.bin = self.tmp / 'bin'
        self.bin.mkdir()
        for name, body in (('sleep', SLEEP), ('ip', IP), ('logger', LOGGER)):
            p = self.bin / name
            p.write_text(body)
            p.chmod(0o755)

    def run_watch(self, budget, carrier=False, env=None):
        self.uptime.write_text('1000.00 0.00\n')
        if self.log.exists():
            self.log.unlink()
        self.link.write_text('<BROADCAST,MULTICAST,UP,LOWER_UP>\n' if carrier else '<BROADCAST,MULTICAST,UP>\n')
        e = dict(os.environ, PATH=f'{self.bin}:{os.environ["PATH"]}', STUBLOG=str(self.log), FAKE_LINK=str(self.link),
                 BUDGET=str(budget), MU5358_GADGET=str(self.g), MU5358_UDC_CLASS=str(self.tmp / 'udc'),
                 MU5358_NETDEV=str(self.netdev), MU5358_UPTIME=str(self.uptime), MU5358_USBTYPE=str(self.usbtype),
                 MU5358_REBOUND=str(self.stamp), **(env or {}))
        subprocess.run(['sh', str(SCRIPT)], env=e, capture_output=True, text=True, timeout=60)
        return self.log.read_text() if self.log.exists() else ''

    def test_a_link_with_a_carrier_is_left_alone(self):
        self.assertEqual(self.run_watch(2000, carrier=True), '')

    def test_the_old_rule_fired_on_the_first_bad_sample(self):
        # the watch as it was: a bad sample right after its 30 s start was a re-enumeration (that is what cut the Mac's
        # enumeration of the 16 GiB flash drive short, 2026-11-01)
        log = self.run_watch(1100, env={'MU5358_BAD_FOR': '0', 'MU5358_GRACE': '0'})
        self.assertIn('re-enumerating', log)

    def test_a_bad_state_has_to_last_a_minute(self):
        # 30 s start + 50 s bad: nothing yet
        self.assertEqual(self.run_watch(1080), '')
        # 30 s start + 70 s bad: once
        log = self.run_watch(1100)
        self.assertEqual(log.count('re-enumerating'), 1, log)
        self.assertIn('configured (carrier 0)', log)

    def test_a_rebind_on_purpose_buys_a_grace_period(self):
        # mu5358-usb bound the gadget at uptime 1025 (the stamp): no re-enumeration before 1025 + 90 s even though the
        # state is bad all along; afterwards the minute counts from the end of the grace
        self.stamp.parent.mkdir(parents=True)
        self.stamp.write_text('1025\n')
        self.assertEqual(self.run_watch(1170), '')
        log = self.run_watch(1190)
        self.assertEqual(log.count('re-enumerating'), 1, log)

    def stall(self, ping_bumps_tx):
        """The Mac keeps sending (RX grows), we send nothing (TX still) - the 2026-10-30 dead sender - or just a quiet
        link with nothing to say. The watch's ping to the lease holder tells them apart: a live sender's TX moves."""
        self.leases = self.tmp / 'leases'
        self.leases.write_text('1793530000 02:35:58:00:00:02 192.168.58.200 mu5358-usb-host *\n')
        ping = self.bin / 'ping'
        bump = 'awk \'{ if ($1 == "usb0:") { $3 += 1; $11 += 1 } print }\' "$MU5358_NETDEV" > "$MU5358_NETDEV.n" && mv "$MU5358_NETDEV.n" "$MU5358_NETDEV"; '
        ping.write_text('#!/bin/sh\necho "ping $*" >> "$STUBLOG"\n' + (bump if ping_bumps_tx else '') + 'exit 0\n')
        ping.chmod(0o755)
        # the Mac's packets keep coming: every sleep adds to RX (the sleep stub does it when FAKE_RX_GROWS is set)
        (self.bin / 'sleep').write_text(SLEEP.replace('exit 0', 'awk \'{ if ($1 == "usb0:") $3 += 2; print }\' "$MU5358_NETDEV" > "$MU5358_NETDEV.n" && mv "$MU5358_NETDEV.n" "$MU5358_NETDEV"\nexit 0'))
        return self.run_watch(1400, carrier=True, env={'MU5358_LEASES': str(self.leases)})

    def test_a_sender_that_is_dead_is_re_enumerated(self):
        log = self.stall(ping_bumps_tx=False)
        self.assertIn('ping', log)
        self.assertIn('sending stalled', log)

    def test_a_quiet_link_that_can_still_send_is_left_alone(self):
        # the boot of 2026-11-01 14:36: the Mac's mDNS kept RX growing, the device had nothing to say for a minute,
        # and the old rule cut a working link
        log = self.stall(ping_bumps_tx=True)
        self.assertIn('192.168.58.200', log)
        self.assertNotIn('re-enumerating', log)

    def test_no_host_on_the_port_means_no_re_enumeration(self):
        self.usbtype.write_text('SDP CDP [DCP]\n')
        self.assertEqual(self.run_watch(1300), '')


if __name__ == '__main__':
    unittest.main()
