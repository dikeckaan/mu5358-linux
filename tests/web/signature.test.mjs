// The web installer's check of a release's signature: node --test tests/web/signature.test.mjs (node >= 20: its
// WebCrypto has Ed25519, and File). The fixture is the real pair of v2026.10.30.6 (work/release/v2026.10.30.6,
// as published), signed by the project's key installer/release.pub; both ways of verifying (WebCrypto, and
// ed25519.js's own arithmetic for browsers without Ed25519 in WebCrypto) must accept it and refuse a changed file.
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";

const TOP = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const WI = path.join(TOP, "site/assets/web-installer");
const { ed25519Verify, verifyEd25519 } = await import(path.join(WI, "ed25519.js"));
const { RELEASE_KEY, RELEASE_PUB, releaseKey, verifySums } = await import(path.join(WI, "releasekey.js"));
const { parseUsign } = await import(path.join(WI, "flow.js"));
const { STR } = await import(path.join(WI, "strings.js"));
const { F, REQUIRED_FILES } = await import(path.join(WI, "github.js"));

// work/release/v2026.10.30.6/SHA256SUMS and SHA256SUMS.sig, byte for byte
const SUMS = `# version v2026.10.30.6
# image mu5358-openwrt.img.gz d9bb25ee338239a6b8f2a65a38a4688ebf4a49ec0a31e6ab376ff53b022d4031 2146435072
1aeab11d3d7b0dbe72c15ad38807a377a0109d84957f4eae04e5cfb01656a4b7  mu5358-openwrt.img.gz
a241ff1048d00862173402902b733220cbcb6a95d7ff901e14a94d739040b9fe  mu5358-kernel.Image.gz
041617a176e1d95970dc4726dd8cf234460ba90293e1257db4d3effa0b673ca9  mu5358-ramdisk.tar.gz
c56249047ece8645d3185d7e2fed5cd8be7453851f2f4630d75e54ea30b24453  mu5358-tools.tar.gz
931a1f5e7732ab02a6316f02a468a0352953a318d810e24976aeaaea6768c5d2  mu5358-install.sh
991bf48144ae02ccee5e8ed725b44c00523a03f56df541810a73de7fce9191cf  android-files.txt
`;
const SIG = `untrusted comment: signed by key 15fd2f702186c19e
RWQV/S9wIYbBnv6aqboLLP5Pi9uEfRgcVk0KuQ6cGZ7Wxhxj7+knqVEap3/2UT1mQ9GRUfXxTadATwDr3723TabPPZghCvBfuw0=
`;
const bytes = (s) => new TextEncoder().encode(s);
const NO_WEBCRYPTO = null;      // verifySums(..., cryptoImpl): without WebCrypto the page's own code does it

test("the embedded key is installer/release.pub, number 15fd2f702186c19e", () => {
  assert.equal(RELEASE_PUB, fs.readFileSync(path.join(TOP, "installer/release.pub"), "utf8"));
  assert.equal(RELEASE_KEY.number, "15fd2f702186c19e");
  assert.equal(RELEASE_KEY.short, "15fd2f70");
  assert.equal(RELEASE_KEY.pub.length, 32);
});

test("parseUsign reads keys and signatures and refuses other files", () => {
  const k = parseUsign(RELEASE_PUB, "key");
  assert.equal(k.keyNumber, "15fd2f702186c19e");
  assert.equal(k.body.length, 32);
  const s = parseUsign(SIG, "signature");
  assert.equal(s.keyNumber, "15fd2f702186c19e");
  assert.equal(s.body.length, 64);
  assert.equal(s.comment, "signed by key 15fd2f702186c19e");
  assert.equal(parseUsign(SIG, "key"), null);                               // a signature is not a key
  assert.equal(parseUsign(SUMS, "signature"), null);                        // no comment line
  assert.equal(parseUsign("untrusted comment: x\nnot base64!\n"), null);
  assert.equal(parseUsign("untrusted comment: x\n" + btoa("Xx" + "a".repeat(72)) + "\n"), null);   // not "Ed"
  assert.notEqual(parseUsign(SIG.replace(/\n/g, "\r\n")), null);           // CRLF, as a Windows download may be
});

test("the real pair verifies, with WebCrypto and with the page's own Ed25519", async () => {
  const w = await verifySums(bytes(SUMS), SIG);
  assert.deepEqual(w, { ok: true, how: "webcrypto", keyNumber: "15fd2f702186c19e" });
  const j = await verifySums(bytes(SUMS), SIG, RELEASE_KEY, NO_WEBCRYPTO);
  assert.deepEqual(j, { ok: true, how: "js", keyNumber: "15fd2f702186c19e" });
  const sig = parseUsign(SIG).body;
  assert.equal(await ed25519Verify(RELEASE_KEY.pub, bytes(SUMS), sig), true);
  assert.deepEqual(await verifyEd25519(RELEASE_KEY.pub, bytes(SUMS), sig), { ok: true, how: "webcrypto" });
});

test("a changed SHA256SUMS fails, both ways", async () => {
  const changed = [SUMS.replace("1aeab11d", "1aeab11e"), SUMS + "# one more line\n", SUMS.slice(0, -1), " " + SUMS];
  for (const c of changed) {
    assert.deepEqual(await verifySums(bytes(c), SIG), { ok: false, why: "sig-invalid", keyNumber: "15fd2f702186c19e" });
    assert.deepEqual(await verifySums(bytes(c), SIG, RELEASE_KEY, NO_WEBCRYPTO), { ok: false, why: "sig-invalid", keyNumber: "15fd2f702186c19e" });
  }
  // a changed signature fails too, and a corrupt S (>= the group order) is refused before any arithmetic
  const sigBytes = parseUsign(SIG).body;
  const flipped = new Uint8Array(sigBytes); flipped[5] ^= 1;
  assert.equal(await ed25519Verify(RELEASE_KEY.pub, bytes(SUMS), flipped), false);
  const bigS = new Uint8Array(sigBytes); bigS.fill(0xff, 32);
  assert.equal(await ed25519Verify(RELEASE_KEY.pub, bytes(SUMS), bigS), false);
});

test("a signature by another key number is refused without verifying", async () => {
  // the same signature, with the key number changed in it
  const raw = Uint8Array.from(atob(SIG.split("\n")[1]), (c) => c.charCodeAt(0));
  raw[9] ^= 0xff;
  const other = "untrusted comment: signed by key 15fd2f702186c161\n" + btoa(String.fromCharCode(...raw)) + "\n";
  assert.deepEqual(await verifySums(bytes(SUMS), other), { ok: false, why: "sig-key", keyNumber: "15fd2f702186c161" });
  // and the real signature, against a key with another number
  const otherKey = { ...RELEASE_KEY, number: "0000000000000000" };
  assert.deepEqual(await verifySums(bytes(SUMS), SIG, otherKey), { ok: false, why: "sig-key", keyNumber: "15fd2f702186c19e" });
  // a file that is not a signature at all
  assert.deepEqual(await verifySums(bytes(SUMS), SUMS), { ok: false, why: "sig-format", keyNumber: "" });
});

test("the demo's release is signed by the demo's key, and ?demo=badsig is not", async () => {
  const demo = await import(path.join(WI, "demo.js"));
  const key = releaseKey(demo.DEMO_PUB);
  assert.notEqual(key.number, RELEASE_KEY.number);
  const fileOf = async (variant, name) => new Uint8Array(await (await demo.demoRelease(variant)).files.find((f) => f.name === name).arrayBuffer());
  const sigText = new TextDecoder().decode(await fileOf("fresh", F.sig));
  assert.equal((await verifySums(await fileOf("fresh", F.sums), sigText, key)).ok, true);
  assert.equal((await verifySums(await fileOf("fresh", F.sums), sigText, key, NO_WEBCRYPTO)).ok, true);
  assert.deepEqual(await verifySums(await fileOf("badsig", F.sums), sigText, key), { ok: false, why: "sig-invalid", keyNumber: key.number });
  // the demo's key never passes for the project's, nor the project's for the demo's
  assert.equal((await verifySums(await fileOf("fresh", F.sums), sigText)).why, "sig-key");
  assert.equal((await verifySums(bytes(SUMS), SIG, key)).why, "sig-key");
});

test("the page requires SHA256SUMS.sig of every release, and the strings for it in every language", () => {
  assert.equal(F.sig, "SHA256SUMS.sig");
  assert.ok(REQUIRED_FILES.includes(F.sig));
  const keys = Object.keys(STR.en);
  for (const k of ["rel.signed", "rel.signed.demo", "file.needsig", "file.sig.ok", "file.bad.sums-unsigned", "file.bad.sig-format", "file.bad.sig-key",
    "file.bad.sig-invalid", "sig.failed.title", "sig.failed.sig-format", "sig.failed.sig-key", "sig.failed.sig-invalid", "sig.failed.text"]) assert.ok(keys.includes(k), k);
  for (const lang of ["tr", "zh"]) assert.deepEqual(Object.keys(STR[lang]).sort(), [...keys].sort(), lang + ": the same keys as en");
});
