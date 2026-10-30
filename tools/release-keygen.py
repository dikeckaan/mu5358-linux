#!/usr/bin/env python3
"""Make the project's release key pair in usign's format, without usign or signify:
    tools/release-keygen.py SECRET_OUT PUBLIC_OUT
SECRET_OUT (mode 600, never in the repository: the maintainer's own, and the Release workflow's secret) is what
tools/sign-release.sh signs SHA256SUMS with; PUBLIC_OUT is installer/release.pub, in the repository, in the web
installer and in every image (/opt/mu5358/lib/release.pub), so that a release is believed only with its signature.
Formats as usign writes them (OpenBSD's signify, no passphrase): a comment line, then base64 of
  secret: "Ed" "BK" rounds(0, 4 bytes) salt(16) checksum(8: sha512 of seed+public) keynum(8) seed(32) public(32)
  public: "Ed" keynum(8) public(32)
The public key is the seed's Ed25519 point (RFC 8032 5.1.5), computed with tools/usign-verify.py's arithmetic.
"""
import base64
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util
spec = importlib.util.spec_from_file_location('usign_verify', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'usign-verify.py'))
uv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(uv)


def public_of(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], 'little')
    a &= (1 << 254) - 8
    a |= 1 << 254
    x, y, z, _t = uv._mul(a, uv._G)
    zi = pow(z, uv.P - 2, uv.P)
    x, y = x * zi % uv.P, y * zi % uv.P
    return (y | ((x & 1) << 255)).to_bytes(32, 'little')


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    sec_path, pub_path = argv[1], argv[2]
    if os.path.exists(sec_path):
        sys.exit('%s exists: not overwriting a release key' % sec_path)
    seed = os.urandom(32)
    pub = public_of(seed)
    keynum = os.urandom(8)
    checksum = hashlib.sha512(seed + pub).digest()[:8]
    sec = b'Ed' + b'BK' + b'\0\0\0\0' + os.urandom(16) + checksum + keynum + seed + pub
    assert len(sec) == 104
    pubblob = b'Ed' + keynum + pub
    num = keynum.hex()
    fd = os.open(sec_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as f:
        f.write(b'untrusted comment: mu5358-linux release secret key ' + num.encode() + b'\n' + base64.b64encode(sec) + b'\n')
    with open(pub_path, 'wb') as f:
        f.write(b'untrusted comment: mu5358-linux release public key ' + num.encode() + b'\n' + base64.b64encode(pubblob) + b'\n')
    print('key %s: secret %s (keep it safe, never commit it), public %s' % (num, sec_path, pub_path))


if __name__ == '__main__':
    main(sys.argv)
