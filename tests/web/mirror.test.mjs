// mirror.js: the browser's own download of a release's files from the project's mirror (a branch of the
// repository on raw.githubusercontent.com). A stand-in fetch serves a made-up release in parts; the module must put
// the files together only when every part and the whole are what the release names, and refuse everything else.
import assert from "node:assert/strict";
import path from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";

const TOP = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const WI = path.join(TOP, "site/assets/web-installer");
const { MirrorError, PART_MAX, downloadFile, downloadRelease, loadMirror, mirrorHas, mirrorUrl } = await import(path.join(WI, "mirror.js"));
const { MIRROR_BASE } = await import(path.join(WI, "config.js"));
const { F, RELEASE_FILES, cleanRelease } = await import(path.join(WI, "github.js"));
const { STR } = await import(path.join(WI, "strings.js"));

const BASE = "https://raw.githubusercontent.com/someone/mu5358-linux/refs/heads/mirror/";
const TAG = "v2026.10.30.7";
const hex = (buf) => Array.from(new Uint8Array(buf), (b) => b.toString(16).padStart(2, "0")).join("");
const sha = async (bytes) => hex(await crypto.subtle.digest("SHA-256", bytes));

// a made-up release: every file some bytes, the image long enough for three parts (PART_MIB = 1 here)
const PART = 1024 * 1024;
async function fixture() {
  const bytes = {};
  for (const name of RELEASE_FILES) {
    const n = name === F.image ? 2 * PART + 12345 : name === F.kernel ? PART : 700;
    const b = new Uint8Array(n);
    for (let i = 0; i < n; i++) b[i] = (i * 7 + name.length) & 0xff;
    bytes[name] = b;
  }
  const files = {};
  const served = {};
  for (const [name, b] of Object.entries(bytes)) {
    const parts = [];
    for (let i = 0, at = 0; at < b.length; i++, at += PART) {
      const piece = b.subarray(at, Math.min(at + PART, b.length));
      const pname = `${name}.${String(i).padStart(3, "0")}`;
      parts.push({ name: pname, size: piece.length, sha256: await sha(piece) });
      served[pname] = piece;
    }
    files[name] = { size: b.length, sha256: await sha(b), parts };
  }
  const manifest = { v: 1, tag: TAG, files };
  const release = cleanRelease({ tag_name: TAG, assets: Object.entries(bytes).map(([name, b]) => ({ name, size: b.length, digest: "sha256:" + files[name].sha256 })), published_at: "2026-10-30T00:00:00Z" });
  return { bytes, manifest, served, release };
}

// a fetch that serves the fixture from BASE + TAG, as a stream; `edit` may change what a part is before it goes out
function fakeFetch(fx, edit = {}) {
  const calls = [];
  return Object.assign(async (url, init) => {
    calls.push(url);
    assert.equal(init.redirect, "error");
    assert.equal(init.credentials, "omit");
    const prefix = `${BASE}${TAG}/`;
    if (!url.startsWith(prefix)) return new Response("", { status: 404 });
    const name = url.slice(prefix.length);
    if (name === "manifest.json") return new Response(JSON.stringify("manifest" in edit ? edit.manifest : fx.manifest), { status: 200 });
    let body = fx.served[name];
    if (!body) return new Response("", { status: 404 });
    if (edit.parts && name in edit.parts) body = edit.parts[name];
    if (edit.status && name in edit.status) return new Response("", { status: edit.status[name] });
    return new Response(new Blob([body]).stream(), { status: 200 });
  }, { calls });
}

test("mirrorUrl: a raw.githubusercontent.com repository address, with or without a branch prefix, nothing else", () => {
  assert.equal(mirrorUrl(BASE, TAG, "manifest.json"), `${BASE}${TAG}/manifest.json`);
  assert.equal(mirrorUrl("https://raw.githubusercontent.com/o/r/", TAG, "x.000"), `https://raw.githubusercontent.com/o/r/${TAG}/x.000`);
  assert.equal(mirrorUrl("https://raw.githubusercontent.com/o/r/refs/heads/mirror/", TAG, "a"), `https://raw.githubusercontent.com/o/r/refs/heads/mirror/${TAG}/a`);
  for (const bad of ["", "http://raw.githubusercontent.com/o/r/", "https://example.com/o/r/", "https://raw.githubusercontent.com/o/r", "https://raw.githubusercontent.com/o/../r/",
    "https://raw.githubusercontent.com/o/r/refs/tags/x/", "https://raw.githubusercontent.com/o/r/x/", null, 5]) {
    assert.equal(mirrorUrl(bad, TAG, "manifest.json"), null, String(bad));
  }
  for (const bad of ["", "x", "v1/..", "v1 2", "../v1", "v" + "1".repeat(60)]) assert.equal(mirrorUrl(BASE, bad, "manifest.json"), null, bad);
  for (const bad of ["", "../x", "a/b", ".hidden", "x".repeat(150)]) assert.equal(mirrorUrl(BASE, TAG, bad), null, bad);
});

test("the configured mirror is a raw.githubusercontent.com address of this project, or empty", () => {
  if (MIRROR_BASE) assert.match(MIRROR_BASE, /^https:\/\/raw\.githubusercontent\.com\/[A-Za-z0-9._-]+\/mu5358-linux(-mirror)?\/(refs\/heads\/[A-Za-z0-9._-]+\/)?$/);
  assert.notEqual(mirrorUrl(MIRROR_BASE || BASE, TAG, "manifest.json"), null);
});

test("loadMirror: the manifest, every value checked for its shape", async () => {
  const fx = await fixture();
  const m = await loadMirror(BASE, TAG, fakeFetch(fx));
  assert.equal(m.tag, TAG);
  assert.deepEqual(Object.keys(m.files).sort(), [...RELEASE_FILES].sort());
  assert.equal(m.files[F.image].parts.length, 3);
  assert.equal(m.files[F.image].parts[2].name, `${F.image}.002`);
  assert.ok(mirrorHas(m, fx.release, RELEASE_FILES));
  // no mirror for the tag, or none at all
  await assert.rejects(loadMirror(BASE, "v2026.01.01.1", fakeFetch(fx)), (e) => e instanceof MirrorError && e.code === "none");
  await assert.rejects(loadMirror("", TAG, fakeFetch(fx)), (e) => e instanceof MirrorError && e.code === "none");
  // a fetch that fails
  await assert.rejects(loadMirror(BASE, TAG, async () => { throw new TypeError("offline"); }), (e) => e.code === "network");
  await assert.rejects(loadMirror(BASE, TAG, async () => new Response("", { status: 500 })), (e) => e.code === "network");
  // manifests to refuse
  const clone = () => JSON.parse(JSON.stringify(fx.manifest));
  const bads = [];
  let x = clone(); x.v = 2; bads.push(x);
  x = clone(); x.tag = "v2026.10.30.8"; bads.push(x);
  x = clone(); x.files = []; bads.push(x);
  x = clone(); x.files["evil.sh"] = x.files[F.list]; bads.push(x);                        // not a file of a release
  x = clone(); x.files[F.image].parts[1].name = `${F.kernel}.000`; bads.push(x);          // another file's part
  x = clone(); x.files[F.image].parts[1].size += 1; bads.push(x);                          // the parts do not add up
  x = clone(); x.files[F.image].parts[0].size = PART_MAX + 1; x.files[F.image].size += PART_MAX + 1 - PART; bads.push(x);
  x = clone(); x.files[F.image].parts[0].sha256 = "zz"; bads.push(x);
  x = clone(); x.files[F.image].sha256 = x.files[F.image].sha256.slice(1); bads.push(x);
  x = clone(); x.files[F.image].parts = []; bads.push(x);
  x = clone(); delete x.files[F.image].parts; bads.push(x);
  bads.push("not json", null, [], { v: 1, tag: TAG });
  for (const bad of bads) {
    const f = typeof bad === "string" ? async () => new Response(bad, { status: 200 }) : fakeFetch(fx, { manifest: bad });
    await assert.rejects(loadMirror(BASE, TAG, f), (e) => e instanceof MirrorError && e.code === "bad-manifest", JSON.stringify(bad).slice(0, 80));
  }
});

test("mirrorHas: only when every file is there with the release's size and SHA-256", async () => {
  const fx = await fixture();
  const m = await loadMirror(BASE, TAG, fakeFetch(fx));
  assert.ok(mirrorHas(m, fx.release, RELEASE_FILES));
  assert.ok(!mirrorHas(null, fx.release, RELEASE_FILES));
  const other = cleanRelease({ tag_name: TAG, assets: [...fx.release.assets.values()].map((a) => ({ name: a.name, size: a.size, digest: a.name === F.kernel ? "sha256:" + "0".repeat(64) : "sha256:" + a.sha256 })), published_at: "2026-10-30T00:00:00Z" });
  assert.ok(!mirrorHas(m, other, RELEASE_FILES));
  assert.ok(mirrorHas(m, other, RELEASE_FILES.filter((n) => n !== F.kernel)));
  const nodigest = cleanRelease({ tag_name: TAG, assets: [...fx.release.assets.values()].map((a) => ({ name: a.name, size: a.size })), published_at: "2026-10-30T00:00:00Z" });
  assert.ok(!mirrorHas(m, nodigest, RELEASE_FILES));
});

test("downloadFile: the parts put together are the file, with progress", async () => {
  const fx = await fixture();
  const fetchImpl = fakeFetch(fx);
  const m = await loadMirror(BASE, TAG, fetchImpl);
  const progress = [];
  const blob = await downloadFile(BASE, m, fx.release.assets.get(F.image), { fetchImpl, onProgress: (d, t) => progress.push([d, t]) });
  assert.equal(blob.size, fx.bytes[F.image].length);
  assert.deepEqual(new Uint8Array(await blob.arrayBuffer()), fx.bytes[F.image]);
  assert.deepEqual(progress[0], [0, fx.bytes[F.image].length]);
  assert.deepEqual(progress[progress.length - 1], [fx.bytes[F.image].length, fx.bytes[F.image].length]);
  assert.ok(progress.every(([d, t], i) => d <= t && (i === 0 || d >= progress[i - 1][0])));
  assert.equal(fetchImpl.calls.filter((u) => u.includes(F.image + ".")).length, 3);
});

test("downloadFile: a part that is not what the mirror or the release names is refused", async () => {
  const fx = await fixture();
  const m = await loadMirror(BASE, TAG, fakeFetch(fx));
  const asset = fx.release.assets.get(F.image);
  const changed = new Uint8Array(fx.served[`${F.image}.001`]); changed[5] ^= 1;
  await assert.rejects(downloadFile(BASE, m, asset, { fetchImpl: fakeFetch(fx, { parts: { [`${F.image}.001`]: changed } }) }), (e) => e.code === "mismatch");
  await assert.rejects(downloadFile(BASE, m, asset, { fetchImpl: fakeFetch(fx, { parts: { [`${F.image}.001`]: fx.served[`${F.image}.001`].subarray(0, 1000) } }) }), (e) => e.code === "truncated");
  const longer = new Uint8Array(PART + 1); longer.set(fx.served[`${F.image}.001`]);
  await assert.rejects(downloadFile(BASE, m, asset, { fetchImpl: fakeFetch(fx, { parts: { [`${F.image}.001`]: longer } }) }), (e) => e.code === "mismatch");
  await assert.rejects(downloadFile(BASE, m, asset, { fetchImpl: fakeFetch(fx, { status: { [`${F.image}.002`]: 404 } }) }), (e) => e.code === "truncated");
  await assert.rejects(downloadFile(BASE, m, asset, { fetchImpl: fakeFetch(fx, { status: { [`${F.image}.002`]: 503 } }) }), (e) => e.code === "network");
  // the release names another file than the mirror holds: nothing is fetched
  const other = { ...asset, sha256: "1".repeat(64) };
  const f = fakeFetch(fx);
  await assert.rejects(downloadFile(BASE, m, other, { fetchImpl: f }), (e) => e.code === "mismatch");
  assert.equal(f.calls.length, 0);
  await assert.rejects(downloadFile(BASE, m, { ...asset, sha256: null }, { fetchImpl: f }), (e) => e.code === "mismatch");
  await assert.rejects(downloadFile(BASE, m, { name: "x.bin", size: 1, sha256: "0".repeat(64) }, { fetchImpl: f }), (e) => e.code === "none");
  assert.equal(f.calls.length, 0);
});

test("downloadFile: cancelled", async () => {
  const fx = await fixture();
  const m = await loadMirror(BASE, TAG, fakeFetch(fx));
  const ac = new AbortController();
  const fetchImpl = async (url, init) => { ac.abort(); if (init.signal.aborted) throw new DOMException("aborted", "AbortError"); return new Response("", { status: 500 }); };
  await assert.rejects(downloadFile(BASE, m, fx.release.assets.get(F.kernel), { fetchImpl, signal: ac.signal }), (e) => e.code === "aborted");
});

test("downloadRelease: every file as a File, named, with progress over all of them", async () => {
  const fx = await fixture();
  const fetchImpl = fakeFetch(fx);
  const m = await loadMirror(BASE, TAG, fetchImpl);
  const progress = [];
  const files = await downloadRelease(BASE, m, fx.release, RELEASE_FILES, { fetchImpl, onProgress: (n, d, t) => progress.push([n, d, t]) });
  assert.deepEqual([...files.keys()], RELEASE_FILES);
  const total = RELEASE_FILES.reduce((s, n) => s + fx.bytes[n].length, 0);
  for (const n of RELEASE_FILES) {
    assert.equal(files.get(n).name, n);
    assert.deepEqual(new Uint8Array(await files.get(n).arrayBuffer()), fx.bytes[n]);
  }
  assert.equal(progress[progress.length - 1][1], total);
  assert.ok(progress.every(([, d, t]) => t === total && d <= t));
});

test("the page has a sentence for every way the mirror can fail, in the three languages", () => {
  for (const lang of ["en", "tr", "zh"]) {
    for (const k of ["rel.mirror.probing", "rel.mirror.ready", "rel.mirror.button", "rel.mirror.downloading", "rel.mirror.cancel", "rel.mirror.again", "rel.mirror.fail.title",
      "rel.mirror.fail.network", "rel.mirror.fail.truncated", "rel.mirror.fail.mismatch", "rel.mirror.fail.bad-manifest", "rel.download.or"]) {
      assert.ok(STR[lang][k], `${lang} ${k}`);
    }
  }
});
