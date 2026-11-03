"""NFC tap-to-connect (docs/superpowers/specs/2026-11-03-kano-safe-package-design.md): the tag tool mu5358-nfc against a
file-backed i2c device (tests/fixtures/nfc_i2c.c, compiled with ASan in the build image), never the hardware."""
import json
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from helpers import BUSYBOX_IMAGE, DOCKER, TOP, docker_busybox, docker_image_ready

IMAGE = os.environ.get('MU5358_TEST_BUILD_IMAGE', 'mu5358-mainline-build')
KEY = 'secretpass1'


def attr(tag, value):
    return struct.pack('>HH', tag, len(value)) + value


def credential(ssid, key=b''):
    """The tag's data area for a hotspot, written independently of the tool (the oracle): an NDEF TLV holding one
    WSC record (a Credential: index, SSID, auth, encryption, key, any MAC; version 1.0 and the WFA vendor
    extension), then the terminator TLV."""
    cred = (attr(0x1026, b'\x01') + attr(0x1045, ssid) +
            attr(0x1003, b'\x00\x20' if key else b'\x00\x01') +
            attr(0x100f, b'\x00\x08' if key else b'\x00\x01') +
            attr(0x1027, key) + attr(0x1020, b'\xff' * 6))
    body = attr(0x104a, b'\x10') + attr(0x100e, cred) + attr(0x1049, b'\x00\x37\x2a\x00\x01\x20')
    kind = b'application/vnd.wfa.wsc'
    msg = bytes([0xd2, len(kind), len(body)]) + kind + body
    return bytes([0x03, len(msg)]) + msg + b'\xfe'


class Tool(unittest.TestCase):
    """mu5358-nfc show|clear|wifi|encode on a 1 KiB tag image."""

    @classmethod
    def setUpClass(cls):
        why = docker_image_ready(IMAGE)
        if why:
            raise unittest.SkipTest(why)
        cls.build = Path(tempfile.mkdtemp(prefix='mu5358-nfc-'))
        r = subprocess.run([DOCKER, 'run', '--rm', '-v', f'{TOP}:/w:ro', '-v', f'{cls.build}:/t', IMAGE, 'gcc', '-std=gnu11',
                            '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I/w/tools/nfc',
                            '-o', '/t/nfc', '/w/tests/fixtures/nfc_i2c.c'], capture_output=True, text=True, timeout=600)
        if r.returncode != 0:
            shutil.rmtree(cls.build, ignore_errors=True)
            raise AssertionError('the tool does not build: ' + r.stderr[-2000:])

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.build, ignore_errors=True)

    def setUp(self):
        self.t = Path(tempfile.mkdtemp(prefix='mu5358-nfc-case-'))
        self.addCleanup(shutil.rmtree, self.t, ignore_errors=True)
        self.image = self.t / 'tag.img'
        self.tag(cc=b'\xe1\x10\x6d\x00')

    def tag(self, cc, sak=0, data=b''):
        b = bytearray(1024)
        b[0x0c:0x10] = cc
        b[0x10:0x10 + len(data)] = data
        b[0x3bf] = sak
        self.image.write_bytes(bytes(b))

    def data(self, n=None):
        b = self.image.read_bytes()
        return b[0x10:0x10 + (n or 512)]

    def nfc(self, *args, stdin='', env=None):
        e = ['-e', 'NFC_TEST_IMAGE=/t/tag.img', '-e', 'ASAN_OPTIONS=detect_leaks=0']
        for k, v in (env or {}).items():
            e += ['-e', f'{k}={v}']
        r = subprocess.run([DOCKER, 'run', '--rm', '-i', '-v', f'{self.build}:/b:ro', '-v', f'{self.t}:/t', *e, IMAGE,
                            '/b/nfc', *args], capture_output=True, text=True, input=stdin, timeout=120)
        self.assertNotIn('AddressSanitizer', r.stderr)
        self.assertNotIn('runtime error', r.stderr)
        return r

    def test_encode_matches_oracle(self):
        r = self.nfc('encode', 'MyNet', 'wpa2', KEY)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), credential(b'MyNet', KEY.encode()).hex())
        r = self.nfc('encode', 'MyNet', 'open')
        self.assertEqual(r.stdout.strip(), credential(b'MyNet').hex())

    def test_wifi_writes_and_reads_back(self):
        r = self.nfc('wifi', 'MyNet', 'wpa2', stdin=KEY + '\n')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), 'written')
        want = credential(b'MyNet', KEY.encode())
        self.assertEqual(self.data(len(want)), want)
        self.assertNotIn(KEY, r.stdout + r.stderr)

    def test_second_write_unchanged(self):
        self.nfc('wifi', 'MyNet', 'wpa2', stdin=KEY)
        before = self.image.read_bytes()
        r = self.nfc('wifi', 'MyNet', 'wpa2', stdin=KEY)
        self.assertEqual(r.stdout.strip(), 'unchanged')
        self.assertEqual(self.image.read_bytes(), before)

    def test_clear(self):
        self.nfc('wifi', 'MyNet', 'wpa2', stdin=KEY)
        r = self.nfc('clear')
        self.assertEqual(r.stdout.strip(), 'written')
        self.assertEqual(self.data(3), b'\x03\x00\xfe')
        self.assertIn('empty NDEF message', self.nfc('show').stdout)

    def test_show_masks_ssid(self):
        self.nfc('wifi', 'MyNet', 'wpa2', stdin=KEY)
        r = self.nfc('show')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('ssid 5 bytes', r.stdout)
        self.assertNotIn('MyNet', r.stdout)
        self.assertIn('auth 0x0020', r.stdout)
        self.assertIn('key 11 characters', r.stdout)
        self.assertIn('SAK 0x00', r.stdout)
        v = self.nfc('show', '-v').stdout
        self.assertIn('ssid MyNet', v)
        self.assertNotIn(KEY, r.stdout + v)

    def test_readonly_cc_refused(self):
        self.tag(cc=b'\xe1\x10\x6d\x0f')
        before = self.image.read_bytes()
        r = self.nfc('wifi', 'MyNet', 'wpa2', stdin=KEY)
        self.assertEqual(r.returncode, 1)
        self.assertIn('not a writable NDEF tag', r.stderr)
        self.assertEqual(self.image.read_bytes(), before)
        self.assertEqual(self.nfc('show').returncode, 0, 'reading a read-only tag is fine')

    def test_bad_key_rejected(self):
        self.assertEqual(self.nfc('wifi', 'MyNet', 'wpa2', stdin='1234567').returncode, 2)
        self.assertEqual(self.nfc('wifi', 'MyNet', 'wpa2', stdin='g' * 64).returncode, 2)
        self.assertEqual(self.nfc('wifi', 'MyNet', 'wpa2', stdin='x' * 63).returncode, 0)
        self.assertEqual(self.nfc('wifi', 'MyNet', 'wpa2', stdin='ab' * 32).returncode, 0, '64 hex digits')

    def test_ssid_bytes_exact(self):
        ssid = 'Café "x" $y'
        r = self.nfc('wifi', ssid, 'open')
        self.assertEqual(r.returncode, 0, r.stderr)
        want = credential(ssid.encode())
        self.assertEqual(self.data(len(want)), want)
        self.assertEqual(self.nfc('wifi', 'x' * 33, 'open').returncode, 2)
        self.assertEqual(self.nfc('wifi', 'x' * 32, 'open').returncode, 0)

    def test_interrupted_write_leaves_empty_ndef(self):
        self.nfc('wifi', 'OldNet', 'wpa2', stdin=KEY)
        old, new = credential(b'OldNet', KEY.encode()), credential(b'NewNet', KEY.encode())
        first = next(i for i, (a, b) in enumerate(zip(old, new)) if a != b)  # the tool skips the pages that do not change
        r = self.nfc('wifi', 'NewNet', 'wpa2', stdin=KEY, env={'NFC_TEST_FAIL_AT': hex(0x10 + first // 4 * 4)})
        self.assertEqual(r.returncode, 1)
        self.assertEqual(self.data(2)[1], 0, 'the NDEF length page was emptied before the body changed')
        r = self.nfc('wifi', 'NewNet', 'wpa2', stdin=KEY)
        self.assertEqual(r.stdout.strip(), 'written', 'the next sync repairs it')
        want = credential(b'NewNet', KEY.encode())
        self.assertEqual(self.data(len(want)), want)

    def test_sak_rewritten_when_not_type2(self):
        self.tag(cc=b'\xe1\x10\x6d\x00', sak=0x20)
        r = self.nfc('wifi', 'MyNet', 'wpa2', stdin=KEY)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.image.read_bytes()[0x3bf], 0)
        log = (self.t / 'tag.img.log').read_text()
        self.assertIn('reg fff4 77', log)
        self.assertIn('reg ffe6 55', log)
        self.assertEqual((self.t / 'tag.img.log').exists() and 'reg' in log, True)
        self.tag(cc=b'\xe1\x10\x6d\x00', sak=0)
        (self.t / 'tag.img.log').unlink()
        self.nfc('wifi', 'MyNet', 'wpa2', stdin=KEY)
        self.assertFalse((self.t / 'tag.img.log').exists(), 'a Type 2 tag is left alone')


SYNC = TOP / 'openwrt/nfc/mu5358-nfc-sync'
INIT = TOP / 'openwrt/nfc/mu5358-nfc.init'
HOTPLUG = TOP / 'openwrt/nfc/95-mu5358-nfc.hotplug'
UCI_STUB = """#!/bin/sh
# uci stand-in: "get KEY" answers from /t/uci.kv (KEY=VALUE lines), exit 1 when absent; anything else is recorded
[ "$1" = -q ] && shift
if [ "$1" = get ]; then
    v=$(sed -n "s/^$2=//p" /t/uci.kv | head -n 1)
    [ -n "$v" ] || exit 1
    echo "$v"
    exit 0
fi
echo "$*" >> /t/uci.log
"""
NFC_STUB = """#!/bin/sh
# mu5358-nfc stand-in: records its arguments and its stdin, answers like the tool
echo "$*" >> /t/argv.log
cut -d' ' -f1 /proc/uptime >> /t/times
[ "$1" = wifi ] && [ "$3" = wpa2 ] && cat > /t/stdin.bin
[ -s /t/nfc.sleep ] && sleep "$(cat /t/nfc.sleep)"
rc=$(cat /t/nfc.rc 2>/dev/null || echo 0)
[ "$rc" = 0 ] && echo written || echo "mu5358-nfc: no tag on /dev/i2c-4 at 0x57: Remote I/O error" >&2
exit "$rc"
"""


class Sync(unittest.TestCase):
    """mu5358-nfc-sync (openwrt/nfc): the one writer of the tag, under busybox with uci and the tool stubbed."""

    @classmethod
    def setUpClass(cls):
        why = docker_busybox()
        if why:
            raise unittest.SkipTest(why)

    def setUp(self):
        self.t = Path(tempfile.mkdtemp(prefix='mu5358-nfc-sync-', dir=TOP / 'work'))
        self.addCleanup(shutil.rmtree, self.t, ignore_errors=True)
        (self.t / 'bin').mkdir()
        (self.t / 'run').mkdir()
        (self.t / 'etc').mkdir()
        (self.t / 'bin/uci').write_text(UCI_STUB)
        (self.t / 'bin/mu5358-nfc').write_text(NFC_STUB)
        for f in ('bin/uci', 'bin/mu5358-nfc'):
            (self.t / f).chmod(0o755)
        (self.t / 'i2c-4').write_text('')
        self.uci(ssid='MyNet', key=KEY, encryption='psk2')

    def uci(self, **kv):
        self.kv = ''.join(f'wireless.ap0.{k}={v}\n' for k, v in kv.items())

    def sh(self, script, timeout=120, rc='0', sleep=''):
        # Every file the script reads is written from inside the container: a host write or unlink just before a
        # docker run can be served stale through the bind mount (seen on Docker Desktop, 2026-11-03).
        prelude = ('export MU5358_NFC_TOOL=/t/bin/mu5358-nfc MU5358_NFC_STATE=/t/etc/nfc MU5358_NFC_RUN=/t/run '
                   'MU5358_UCI=/t/bin/uci MU5358_NFC_BUS=/t/i2c-4 MU5358_NFC_SYNC=/w/openwrt/nfc/mu5358-nfc-sync PATH=/t/bin:$PATH; '
                   'S=/w/openwrt/nfc/mu5358-nfc-sync; : > /t/argv.log; : > /t/times; printf "%s" "$KV" > /t/uci.kv; '
                   'printf "%s" "$NFC_RC" > /t/nfc.rc; printf "%s" "$NFC_SLEEP" > /t/nfc.sleep; ')
        argv = [DOCKER, 'run', '--rm', '-e', f'KV={self.kv}', '-e', f'NFC_RC={rc}', '-e', f'NFC_SLEEP={sleep}',
                '-v', f'{TOP}:/w:ro', '-v', f'{self.t}:/t', BUSYBOX_IMAGE, 'sh', '-c', prelude + script]
        return subprocess.run(argv, capture_output=True, text=True, timeout=timeout)

    def argv(self):
        p = self.t / 'argv.log'
        return p.read_text().splitlines() if p.exists() else []

    def result(self):
        return (self.t / 'run/nfc-result').read_text().strip()

    def test_default_on_writes_wpa2(self):
        r = self.sh('sh $S sync')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.argv(), ['wifi MyNet wpa2'])
        self.assertEqual((self.t / 'stdin.bin').read_bytes(), KEY.encode())
        self.assertEqual(self.result(), 'wpa2')
        self.assertEqual(self.sh('sh $S state').stdout.strip(), 'on')

    def test_key_only_on_stdin(self):
        key = '-start $x `y` \\z"q'
        self.uci(ssid='My Net "x"', key=key, encryption='psk2')
        r = self.sh('sh $S sync')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.argv(), ['wifi My Net "x" wpa2'])
        self.assertEqual((self.t / 'stdin.bin').read_bytes(), key.encode())
        self.assertNotIn(key, (self.t / 'argv.log').read_text())
        self.assertNotIn('$x', r.stdout + r.stderr)

    def test_off_clears(self):
        r = self.sh('echo off > /t/etc/nfc; sh $S sync')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.argv(), ['clear'])
        self.assertEqual(self.result(), 'disabled')
        self.assertEqual(self.sh('sh $S state').stdout.strip(), 'off')

    def test_hotspot_disabled_clears(self):
        self.uci(ssid='MyNet', key=KEY, encryption='psk2', disabled='1')
        self.sh('sh $S sync')
        self.assertEqual(self.argv(), ['clear'])
        self.assertEqual(self.result(), 'hotspot_off')

    def test_unsupported_encryption_clears(self):
        for enc in ('sae', 'psk', 'owe', 'wpa2+eap', 'psk-mixed'):
            self.uci(ssid='MyNet', key=KEY, encryption=enc)
            self.sh('sh $S sync')
            self.assertEqual(self.argv(), ['clear'], enc)
            self.assertEqual(self.result(), 'unsupported', enc)
        for enc, want in (('none', 'wifi MyNet open'), ('', 'wifi MyNet open'), ('sae-mixed', 'wifi MyNet wpa2'),
                          ('psk2+ccmp', 'wifi MyNet wpa2'), ('psk2+aes', 'wifi MyNet wpa2')):
            self.uci(ssid='MyNet', key=KEY, encryption=enc)
            self.sh('sh $S sync')
            self.assertEqual(self.argv(), [want], enc)
            self.assertEqual(self.result(), want.split()[-1] if want.endswith('open') else 'wpa2', enc)

    def test_toggle_on_off_state(self):
        self.assertEqual(self.sh('sh $S toggle; sh $S state').stdout.strip(), 'off')
        self.assertEqual((self.t / 'etc/nfc').read_text().strip(), 'off')
        self.assertEqual(self.sh('sh $S toggle; sh $S state').stdout.strip(), 'on')
        self.assertFalse((self.t / 'etc/nfc').exists(), 'absent = on')
        self.assertEqual(self.sh('sh $S off; sh $S state').stdout.strip(), 'off')
        self.assertEqual(self.sh('sh $S on; sh $S state').stdout.strip(), 'on')
        self.assertEqual(self.argv()[-1], 'wifi MyNet wpa2', 'every switch re-syncs')

    def test_tool_failure_is_error(self):
        r = self.sh('sh $S sync', rc='1')
        self.assertEqual(r.returncode, 1)
        self.assertEqual(self.result(), 'error')
        err = self.t / 'run/nfc.err'
        self.assertTrue(err.exists())
        self.assertEqual(err.stat().st_mode & 0o777, 0o600)
        self.assertIn('Remote I/O error', err.read_text())

    def test_no_tool_is_no_tag(self):
        r = self.sh('MU5358_NFC_TOOL=/t/none sh $S sync')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.result(), 'no_tag')
        self.assertEqual(self.argv(), [])
        self.assertEqual(self.sh('MU5358_NFC_TOOL=/t/none sh $S supported; echo rc=$?').stdout.strip(), 'rc=1')
        self.assertEqual(self.sh('sh $S supported; echo rc=$?').stdout.strip(), 'rc=0')
        self.assertEqual(self.sh('MU5358_NFC_BUS=/t/none sh $S supported; echo rc=$?').stdout.strip(), 'rc=1', 'no bus')

    def test_lock_serialises(self):
        r = self.sh('sh $S sync & sh $S sync & wait', sleep='1')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(len(self.argv()), 2)
        self.assertEqual(self.result(), 'wpa2')
        self.assertTrue((self.t / 'run/nfc.lock').exists())
        a, b = (float(x) for x in (self.t / 'times').read_text().split())
        self.assertGreaterEqual(abs(a - b), 0.9, 'the second sync waited for the first')

    def test_init_and_hotplug_shape(self):
        init = INIT.read_text()
        self.assertIn('START=95', init)
        self.assertIn('procd_add_reload_trigger wireless', init)
        self.assertNotIn('respawn', init)
        self.assertIn('mu5358-nfc-sync sync', init)
        hp = HOTPLUG.read_text()
        for action, iface, want in (('ifup', 'lan', 1), ('ifdown', 'lan', 1), ('ifup', 'wan', 0), ('ifupdate', 'lan', 0)):
            r = self.sh(f'export ACTION={action} INTERFACE={iface}; sh /w/openwrt/nfc/95-mu5358-nfc.hotplug')
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertEqual(len(self.argv()), want, (action, iface))
        self.uci(ssid='MyNet', key=KEY, encryption='psk2', network='guest')
        self.sh('export ACTION=ifup INTERFACE=lan; sh /w/openwrt/nfc/95-mu5358-nfc.hotplug')
        self.assertEqual(self.argv(), [], 'the AP on another network: lan is not it')
        self.sh('export ACTION=ifup INTERFACE=guest; sh /w/openwrt/nfc/95-mu5358-nfc.hotplug')
        self.assertEqual(len(self.argv()), 1)


RPCD = TOP / 'openwrt/nfc/rpcd-mu5358nfc'
ACL = TOP / 'openwrt/nfc/acl-mu5358nfc.json'
RECORDER = """#!/bin/sh
# mu5358-nfc-sync stand-in: records its arguments, answers state/supported from files
printf '%s\\n' "$*" >> "$STUBLOG"
case $1 in
    state) cat "$STUB_STATE" ;;
    supported) exit "$(cat "$STUB_SUPPORTED")" ;;
esac
exit "${STUB_RC:-0}"
"""


class Rpcd(unittest.TestCase):
    """The rpcd object mu5358nfc (openwrt/nfc/rpcd-mu5358nfc) over a recorded stand-in for mu5358-nfc-sync."""

    def setUp(self):
        self.t = Path(tempfile.mkdtemp(prefix='mu5358-nfc-rpcd-'))
        self.addCleanup(shutil.rmtree, self.t, ignore_errors=True)
        (self.t / 'run').mkdir()
        self.sync = self.t / 'mu5358-nfc-sync'
        self.sync.write_text(RECORDER)
        self.sync.chmod(0o755)
        self.log = self.t / 'stub.log'
        (self.t / 'state').write_text('on\n')
        (self.t / 'supported').write_text('0')
        (self.t / 'run/nfc-result').write_text('wpa2\n')
        self.env = dict(os.environ, MU5358_NFC_SYNC=str(self.sync), MU5358_NFC_RUN=str(self.t / 'run'), STUBLOG=str(self.log),
                        STUB_STATE=str(self.t / 'state'), STUB_SUPPORTED=str(self.t / 'supported'))

    def rpc(self, *args, stdin=None):
        r = subprocess.run(['sh', str(RPCD), *args], env=self.env, capture_output=True, text=True, input=stdin, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        return json.loads(r.stdout)

    def calls(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_list(self):
        self.assertEqual(self.rpc('list'), {'get': {}, 'set': {'value': 'str'}})

    def test_get(self):
        self.assertEqual(self.rpc('call', 'get'), {'ok': 1, 'state': 'on', 'supported': 1, 'result': 'wpa2'})
        (self.t / 'state').write_text('off\n')
        (self.t / 'supported').write_text('1')
        (self.t / 'run/nfc-result').unlink()
        self.assertEqual(self.rpc('call', 'get'), {'ok': 1, 'state': 'off', 'supported': 0, 'result': ''})
        (self.t / 'run/nfc-result').write_text('weird words here\n')
        self.assertEqual(self.rpc('call', 'get')['result'], '', 'only the known words pass')

    def test_set_runs_the_sync(self):
        out = self.rpc('call', 'set', stdin='{"value":"off"}')
        self.assertEqual(out, {'ok': 1})
        self.assertEqual(self.calls(), ['off'])
        self.rpc('call', 'set', stdin='{ "value": "on" }')
        self.assertEqual(self.calls(), ['off', 'on'])

    def test_refuses_what_is_not_allowed(self):
        for body in ('{"value":"on; reboot"}', '{"value":"toggle"}', '{"value":"sync"}', '{"kind":"on"}', 'garbage', ''):
            out = self.rpc('call', 'set', stdin=body)
            self.assertEqual(out['ok'], 0, body)
            self.assertEqual(out['error'], 'nfc_value')
        self.assertEqual(self.calls(), [])

    def test_a_failed_sync_is_an_error(self):
        self.env['STUB_RC'] = '1'
        out = self.rpc('call', 'set', stdin='{"value":"on"}')
        self.assertEqual(out['ok'], 0)
        self.assertEqual(out['error'], 'nfc_failed')

    def test_acl_names_the_methods(self):
        acl = json.loads(ACL.read_text())
        self.assertEqual(acl['luci-app-mu5358']['read']['ubus']['mu5358nfc'], ['get'])
        self.assertEqual(acl['luci-app-mu5358']['write']['ubus']['mu5358nfc'], ['set'])


if __name__ == '__main__':
    unittest.main()
