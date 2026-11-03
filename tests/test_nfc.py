"""NFC tap-to-connect (docs/superpowers/specs/2026-11-03-kano-safe-package-design.md): the tag tool mu5358-nfc against a
file-backed i2c device (tests/fixtures/nfc_i2c.c, compiled with ASan in the build image), never the hardware."""
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from helpers import DOCKER, TOP, docker_image_ready

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


if __name__ == '__main__':
    unittest.main()
