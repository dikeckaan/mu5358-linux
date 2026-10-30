// sha512crypt.js: the page's crypt(3) $6$, against Ulrich Drepper's own test vector ("Unix crypt using SHA-256 and
// SHA-512", 2007) and one of OpenSSL's (`openssl passwd -6 -salt saltsalt test`; the example in the file's own header
// comment is not what crypt(3) gives for "test"/"saltsalt"); what the installer's `password` accepts is what the
// page makes.
import test from "node:test";
import assert from "node:assert/strict";

import { isHash, isSalt, makeSalt, sha512crypt } from "../../site/assets/web-installer/sha512crypt.js";

const VECTORS = [
  ["saltstring", "Hello world!", "$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1"],
  ["saltsalt", "test", "$6$saltsalt$JcVDtuB6d1BHhCd5RPBh8g8xX/1CbY8EU2PN0MTaj2/Mypw4P./C6dN4j0HALhzBDTocyW1Jm.gYaTPjFGCV40"],
];
// the installer's own rule (installer/mu5358-install.sh, password())
const DEVICE = /^\$6\$[./A-Za-z0-9]{1,16}\$[./A-Za-z0-9]{86}$/;

test("sha512crypt: Drepper's vector and OpenSSL's", async () => {
  for (const [salt, pw, want] of VECTORS) {
    assert.equal(await sha512crypt(pw, salt), want);
    assert.ok(DEVICE.test(want), "the device takes it");
    assert.ok(isHash(want));
  }
});

test("sha512crypt: a different password or salt gives a different hash; a salt longer than 16 is refused", async () => {
  const a = await sha512crypt("Hello world!", "saltstring");
  assert.notEqual(await sha512crypt("Hello world?", "saltstring"), a);
  assert.notEqual(await sha512crypt("Hello world!", "saltstrinG"), a);
  await assert.rejects(sha512crypt("x", "a".repeat(17)), /bad salt/);
  await assert.rejects(sha512crypt("x", "salt$"), /bad salt/);
  await assert.rejects(sha512crypt("x", ""), /bad salt/);
});

test("sha512crypt: a made salt is 16 characters of the alphabet, and the hash of a Unicode password is one the device takes", async () => {
  for (let i = 0; i < 20; i++) {
    const s = makeSalt();
    assert.equal(s.length, 16);
    assert.ok(isSalt(s), s);
  }
  const h = await sha512crypt("şifre 密码 😀");
  assert.ok(DEVICE.test(h), h);
  assert.ok(isHash(h));
});

test("isHash: only a $6$ hash of the right shape", () => {
  const ok = VECTORS[0][2];
  assert.ok(isHash(ok));
  for (const bad of ["", "x", ok.slice(0, -1), ok + "a", "$5$" + ok.slice(3), " " + ok, ok + "\n", ok.replace("/", "!"),
    "$6$" + "a".repeat(17) + "$" + ok.split("$")[3], "$6$$" + ok.split("$")[3], null, undefined, 5]) {
    assert.ok(!isHash(bad), String(bad));
  }
});
