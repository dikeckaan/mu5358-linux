// The page without a device: ?demo (a fresh U30 Pro), ?demo=installed, ?demo=running (a job left by an earlier
// visit), ?demo=old (a script from before the region), ?demo=refused (not a U30 Pro, no magiskboot), ?demo=badsig
// (a release whose SHA256SUMS was changed after it was signed: GitHub's digest matches the changed file, the
// project's signature does not, and the page must refuse it).
// Made-up answers behind the same calls as device.js, and a made-up release whose files are real (small) Blobs with
// a real SHA256SUMS and a real signature of it, so that the page's checks run for real. Nothing here opens a USB
// device or writes anything. Loaded only when the address asks for it.
import { F, filesOf } from "./github.js";
import { sha256Hex } from "./sha256.js";

const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const MIB = 1024 * 1024, GIB = 1024 * MIB;
export const DEMO_TAG = "v2026.10.30.2";

// The demo's release key and its signature of the demo's SHA256SUMS (the text demoSumsText() makes, which only
// depends on the contents below). The real releases are signed by the project's key (releasekey.js), whose secret
// half is not on any web page, so the demo has a key of its own, made once with Node's crypto and thrown away; the
// page checks the demo's signature exactly as it checks a real one, against this key instead. A change to the demo's
// files or tag breaks the signature (tests/web/signature.test.mjs says so): sign again, or put the old text back.
export const DEMO_PUB = `untrusted comment: mu5358-linux web installer demo key dba741ade429f232
RWTbp0Gt5CnyMv4jXkgdc2l2OldHSXGwTGnrtCHSPAjNXQF3Dcr6g/mA
`;
const DEMO_SIG = `untrusted comment: signed by key dba741ade429f232
RWTbp0Gt5CnyMrt5v+Dk5vmraEIwo9jjqSEnh6aM+RTfltgrIFvrPa7ZyBoZ8XLkevAOpystaAqdcB2D4eNgqBRNh5PlG1m2jQA=
`;

const CONTENTS = {
  [F.image]: "demo: the OpenWrt image (the real one is about 53 MB)\n".repeat(400),
  [F.kernel]: "demo: the kernel\n".repeat(300),
  [F.ramdisk]: "demo: the ramdisk\n".repeat(100),
  [F.tools]: "demo: gpt-userdata, f2fs-shrink\n".repeat(50),
  [F.script]: "#!/system/bin/sh\n# demo: the device script\n",
  [F.list]: "system/lib64/libc++.so\n",
};
const demoBlobs = () => new Map(Object.entries(CONTENTS).map(([name, c]) => [name, new TextEncoder().encode(c)]));

// the demo's SHA256SUMS, the text DEMO_SIG signs; "badsig": with a line added after the signing
export async function demoSumsText(variant = "fresh") {
  const lines = [`# version ${DEMO_TAG}`, `# image ${F.image} ${"1".repeat(64)} 2146435072`];
  for (const [name, b] of demoBlobs()) lines.push(`${await sha256Hex(b)}  ${name}`);
  if (variant === "badsig") lines.push("# changed after the signing: the page must refuse this file");
  return lines.join("\n") + "\n";
}

const made = new Map();
// { release, files: [File] }: the release as loadReleases() gives it, and the files a user would have downloaded
export async function demoRelease(variant = "fresh") {
  if (made.has(variant)) return made.get(variant);
  const enc = new TextEncoder();
  const blobs = demoBlobs();
  blobs.set(F.sums, enc.encode(await demoSumsText(variant)));
  blobs.set(F.sig, enc.encode(DEMO_SIG));
  const assets = new Map();
  for (const [name, b] of blobs) {
    assets.set(name, { name, size: b.length, sha256: await sha256Hex(b), url: `https://github.com/dikeckaan/mu5358-linux/releases/download/${DEMO_TAG}/${name}` });
  }
  const release = { tag: DEMO_TAG, prerelease: false, published: Date.now(), assets, page: `https://github.com/dikeckaan/mu5358-linux/releases/tag/${DEMO_TAG}` };
  const files = filesOf(release).map((n) => new File([blobs.get(n)], n, { type: "application/octet-stream" }));
  made.set(variant, { release, files });
  return made.get(variant);
}

function baseStatus(kind) {
  const installed = kind === "installed" || kind === "running";
  const st = {
    model: kind === "refused" ? "SM-A525F" : "MU5358", device: kind === "refused" ? "a52q" : "MU5358", platform: kind === "refused" ? "lito" : "ums9632",
    serial: "324761041479", android: "14", build: "MU5358_B19", slot: "_a", magisk: "28.1:MAGISK:R", magiskboot: kind !== "refused",
    misc: "android", boot_b: installed ? "linux" : "empty", fulldumpdb: "empty", fulldumpdb_size: 2048 * MIB,
    installed: installed ? "v2026.10.30.1" : "", backup: installed, job: kind === "running" ? "running" : "none", free_data: 18 * GIB,
  };
  if (kind !== "old") {
    const last = 61071326;
    st.region = installed ? { state: "ours", start: last + 1 - 4096 * 2048, size: (4096 - 1) * MIB } : { state: "absent", start: 0, size: 0 };
    const udSize = installed ? 26 * GIB : 29 * GIB;
    st.userdata = { start: 10485760, size: udSize, kernel_size: udSize };
    st.root = installed ? "region" : "";
  }
  return st;
}

class DemoDevice {
  constructor(kind, log) {
    this.variant = kind || "fresh";
    this.log = log;
    this.st = baseStatus(this.variant);
    this.script = "";
    this.taken = false;
    this.job = this.variant === "running" ? { kind: "install", args: ["region", "4096"], resumeAt: 4 } : null;
    this.serial = this.st.serial;
  }
  async probe() {
    await wait(200);
    const s = this.st;
    return { props: { "ro.product.model": s.model, "ro.product.device": s.device, "ro.board.platform": s.platform, "ro.boot.slot_suffix": s.slot,
      "ro.build.version.release": s.android, "ro.build.display.id": s.build, "sys.boot_completed": "1" }, su: true };
  }
  async root() { await wait(600); return "granted"; }
  async findRootCopy() { await wait(150); if (this.st.installed) { if (!this.script) this.script = "/data/adb/mu5358/mu5358-install.sh"; return true; } return false; }
  async pushScripts() { await wait(250); this.script = "/data/local/tmp/mu5358-web/mu5358-install.sh"; }
  async pushRelease(files, onProgress = () => {}) {
    const names = [F.sums, F.kernel, F.ramdisk, F.image, F.tools].filter((n) => files.has(n));
    // as if it were the real release: about 70 MB at 20 MB/s
    const total = 70 * 1000 * 1000;
    for (let i = 0; i <= 35; i++) { onProgress(names[Math.min(names.length - 1, Math.floor(i / 9))], total * i / 35, total); await wait(70); }
  }
  async status() {
    await wait(300);
    const s = JSON.parse(JSON.stringify(this.st));
    this.log("MU5358-STATUS " + JSON.stringify(s));
    return s;
  }
  async take() {
    for (const n of [F.kernel, F.ramdisk, F.image, F.tools]) { await wait(250); this.log(`MU5358-INFO ${n} ok`); }
    this.log(`MU5358-TAKEN ${DEMO_TAG}`);
    this.taken = true;
    return DEMO_TAG;
  }
  // the real script keeps the hash for the next run install and refuses to install without one
  async password(hash) {
    await wait(200);
    if (!/^\$6\$[./0-9A-Za-z]{1,16}\$[./0-9A-Za-z]{86}$/.test(hash)) { this.log("MU5358-ERROR not a sha512-crypt hash"); throw Object.assign(new Error("bad hash"), { name: "DeviceError", code: "script", detail: "not a sha512-crypt hash" }); }
    this.log("[password hash]");
    this.pwSet = true;
    this.log("MU5358-PASSWORD set");
  }
  async run(kind, ...args) {
    await wait(300);
    if (this.st.job === "running") { this.log("MU5358-ERROR a job is running"); throw Object.assign(new Error("a job is running"), { name: "DeviceError", code: "script", detail: "a job is running" }); }
    if (kind === "install" && !this.pwSet) { this.log("MU5358-ERROR no password set"); throw Object.assign(new Error("no password set"), { name: "DeviceError", code: "script", detail: "no password set" }); }
    if (kind === "install") this.pwSet = false;
    this.job = { kind, args, resumeAt: 0 };
    this.st.job = "running";
    this.log("MU5358-JOB started");
  }
  async follow(onEvent) {
    const j = this.job;
    if (!j) throw Object.assign(new Error("no job"), { name: "DeviceError", code: "script", detail: "no job" });
    const say = async (line, ms = 350) => {
      const m = /^MU5358-STEP (\S+)/.exec(line), e = /^MU5358-EXIT (\d+)/.exec(line);
      onEvent(m ? { type: "step", name: m[1] } : e ? { type: "exit", code: +e[1] } : line === "MU5358-REBOOT-NEEDED" ? { type: "reboot-needed" }
        : line.startsWith("MU5358-ERROR") ? { type: "error", text: line.slice(13) } : { type: "text", text: line }, line);
      await wait(ms);
    };
    const end = async (code) => { this.st.job = "exit-" + code; await say("MU5358-EXIT " + code, 50); return code; };
    if (j.kind === "uninstall") {
      for (const s of ["misc", "boot", "erase"]) await say("MU5358-STEP " + s, 600);
      if (this.st.region && this.st.region.state === "ours") await say("the region: label erased");
      if (this.st.region && j.args[0] !== "keep-space") {
        await say("the region goes back to userdata at the next start of Android");
        this.st.region = { state: "absent", start: 0, size: 0 };
      } else if (this.st.region) this.st.region.state = "free";
      await say("MU5358-STEP done");
      Object.assign(this.st, { installed: "", boot_b: "android", root: "", fulldumpdb: "empty" });
      return end(0);
    }
    const target = j.args[0] || "region", mib = Number(j.args[1] || 4096);
    const steps = ["check", "backup", "space", "chain", "system", "vendor", "boot", "done"];
    for (let i = j.resumeAt || 0; i < steps.length; i++) {
      const s = steps[i];
      await say("MU5358-STEP " + s, 500);
      if (s === "backup") { await say(this.st.backup ? "kept from 2026-10-30T10:12:00Z" : "boot_b, init_boot_b, misc, GPT copied"); this.st.backup = true; }
      if (s === "space" && target === "region") {
        const r = this.st.region;
        if (r.state === "absent") {
          await say(`userdata: f2fs 7602176 -> ${7602176 - mib * 256} blocks of 4 KiB; Linux gets ${mib} MiB`, 900);
          this.st.userdata.size -= mib * MIB;
          Object.assign(r, { state: "free", start: 61071327 - mib * 2048, size: (mib - 1) * MIB });
          await say("MU5358-REBOOT-NEEDED");
          await say("room made; Android must start once with the new table, then run the installation again");
          j.resumeAt = 0;
          return end(3);
        }
        await say(`the region: ${Math.round(r.size / MIB)} MiB (${r.state})`);
      }
      if (s === "chain") await say("slot b: 4 of 23 partitions differ from slot a: copied, read back", 700);
      if (s === "system") { await say("writing the system (2047 MiB)", 2200); }
      if (s === "vendor") await say("vendor files: 118 copied from this device", 800);
      if (s === "boot") await say("boot_b, init_boot_b written, read back", 800);
    }
    Object.assign(this.st, { installed: DEMO_TAG, boot_b: "linux", root: target });
    if (target === "region") this.st.region.state = "ours"; else this.st.fulldumpdb = "ours";
    await say(`installed ${DEMO_TAG}; Linux starts with boot-linux, once`);
    return end(0);
  }
  async reboot() {
    await wait(400);
    if (this.st.userdata) this.st.userdata.kernel_size = this.st.userdata.size;
  }
  async bootLinux(dry) {
    await wait(500);
    if (dry) { this.log("MU5358-INFO trial 5f620000424342410100000000000000ee002f0000000000000000005d1c4a3b"); return { info: "trial" }; }
    this.log("MU5358-REBOOT");
    return { rebooting: true };
  }
  async backupList() {
    await wait(200);
    const list = [{ name: "boot_b.img.gz", size: 65536 }, { name: "init_boot_b.img.gz", size: 8192 }, { name: "misc-4k.bin", size: 4096 }];
    this.backupBytes = new Map(list.map((f) => [f.name, new Uint8Array(f.size)]));
    const sums = [];
    for (const [n, b] of this.backupBytes) sums.push(`${await sha256Hex(b)}  ${n}`);
    this.backupBytes.set("SHA256SUMS", new TextEncoder().encode(sums.join("\n") + "\n"));
    list.push({ name: "SHA256SUMS", size: this.backupBytes.get("SHA256SUMS").length });
    for (const f of list) this.log(`MU5358-FILE ${f.name} ${f.size}`);
    return list;
  }
  async cat(name) { await wait(300); return this.backupBytes.get(name); }
}

export function makeDemo(kind, log) { return new DemoDevice(kind, log); }
