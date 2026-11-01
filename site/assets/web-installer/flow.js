// What the page decides by itself, without the screen or the device: reading SHA256SUMS and the device script's
// lines, what the status means, which targets and sizes are offered. The device script (installer/mu5358-install.sh)
// checks everything again and has the last word; this is for telling the user before anything is sent.

export const MIB = 1024 * 1024, GIB = 1024 * MIB;
export const INSTALL_STEPS = ["check", "backup", "space", "chain", "system", "vendor", "boot", "done"];
export const UNINSTALL_STEPS = ["misc", "boot", "erase", "done"];
export const REGION_SIZES = [4096, 8192, 16384];     // MiB
export const REGION_DEFAULT = 4096;
export const REGION_MIN = 2560;                      // MiB: the script's minimum
export const ANDROID_KEEPS = 4 * GIB;                 // free space /data keeps after a shrink (the script's MIN_ANDROID_FREE)
export const ROOT_MIB = 2047;                         // the system's filesystem: fulldumpdb less its last MiB
export const DATA_NEED = 600 * MIB;                   // what the script wants free on /data (check)

// SHA256SUMS of a release: "# version TAG", "# image NAME SHA SIZE", then sha256sum lines
export function parseSums(textValue) {
  const out = { version: "", image: null, files: new Map() };
  for (const raw of String(textValue).split("\n")) {
    const l = raw.replace(/\r$/, "").trim();
    let m;
    if ((m = /^# version (\S+)$/.exec(l))) { if (!out.version) out.version = m[1]; continue; }
    if ((m = /^# image (\S+) ([0-9a-f]{64}) (\d+)$/.exec(l))) { out.image = { name: m[1], sha256: m[2], size: Number(m[3]) }; continue; }
    if ((m = /^([0-9a-f]{64}) [ *](\S+)$/.exec(l))) out.files.set(m[2], m[1]);
  }
  return out;
}

// A usign file (OpenBSD signify's format), as usign -G writes a public key and usign -S a signature: a line
// "untrusted comment: ...", then base64 of "Ed", the key's 8-byte number and the 32-byte public key (a key) or the
// 64-byte Ed25519 signature (a signature). -> { comment, keyNumber: 16 hex digits, body: Uint8Array(32 or 64) },
// or null when text is not one. kind: "key" | "signature".
export function parseUsign(textValue, kind = "signature") {
  const lines = String(textValue).split("\n").map((l) => l.replace(/\r$/, ""));
  if (lines.length < 2 || !lines[0].startsWith("untrusted comment:")) return null;
  const b64 = lines[1].trim();
  if (!/^[A-Za-z0-9+/]+={0,2}$/.test(b64) || b64.length % 4 !== 0) return null;
  let bin;
  try { bin = atob(b64); } catch (e) { return null; }
  const raw = Uint8Array.from(bin, (c) => c.charCodeAt(0));
  const size = kind === "key" ? 42 : 74;
  if (raw.length !== size || raw[0] !== 0x45 || raw[1] !== 0x64) return null;     // "Ed"
  let keyNumber = "";
  for (const b of raw.subarray(2, 10)) keyNumber += b.toString(16).padStart(2, "0");
  return { comment: lines[0].slice("untrusted comment:".length).trim(), keyNumber, body: raw.subarray(10) };
}

const STR = (v) => (typeof v === "string" ? v : "");
const NUM = (v) => (Number.isFinite(Number(v)) && v !== null && v !== "" ? Number(v) : null);
const oneOf = (v, list) => (list.includes(v) ? v : "unknown");

// "MU5358-STATUS {json}" -> the status, every field checked; null when the line is not one
export function parseStatus(line) {
  const m = /^MU5358-STATUS (\{.*\})\s*$/.exec(String(line));
  if (!m) return null;
  let o;
  try { o = JSON.parse(m[1]); } catch (e) { return null; }
  if (!o || typeof o !== "object") return null;
  const job = STR(o.job);
  const st = {
    model: STR(o.model), device: STR(o.device), platform: STR(o.platform), serial: STR(o.serial), android: STR(o.android),
    build: STR(o.build), slot: STR(o.slot), magisk: STR(o.magisk), magiskboot: o.magiskboot === true,
    misc: oneOf(o.misc, ["android", "trial", "other", "invalid"]),
    boot_b: oneOf(o.boot_b, ["empty", "linux", "android", "other", "zero"]),
    fulldumpdb: oneOf(o.fulldumpdb, ["empty", "ours", "used", "absent"]), fulldumpdb_size: NUM(o.fulldumpdb_size) || 0,
    installed: STR(o.installed), backup: o.backup === true,
    job: job === "none" || job === "running" || /^exit-\d+$/.test(job) ? job : "none",
    free_data: NUM(o.free_data),
    // newer scripts: the region made from userdata, and where the installed system is
    region: null, userdata: null, root: "",
  };
  if (o.region && typeof o.region === "object") {
    st.region = { state: oneOf(o.region.state, ["absent", "ours", "free"]), start: NUM(o.region.start), size: NUM(o.region.size) };
  }
  if (o.userdata && typeof o.userdata === "object") {
    st.userdata = { start: NUM(o.userdata.start), size: NUM(o.userdata.size), kernel_size: NUM(o.userdata.kernel_size) };
  }
  if (o.root === "region" || o.root === "fulldumpdb") st.root = o.root;
  return st;
}

export const isMu5358 = (st) => /MU5358/i.test(st.model + " " + st.device) || /U30\s*Pro/i.test(st.model);
// the partition table was changed in this Android's boot (a shrink): Android must start once before anything else
export const rebootPending = (st) => !!(st && st.userdata && st.userdata.size !== null && st.userdata.kernel_size !== null &&
  st.userdata.size !== st.userdata.kernel_size);
// "update" where a system is installed and the user points at it (the script keeps the settings, wants no
// password); "install" everywhere else (fresh, a password)
export const jobKind = (st, target) => (st && st.installed && st.root === target ? "update" : "install");
export const jobRunning = (st) => !!st && st.job === "running";
export const jobExit = (st) => (st && /^exit-(\d+)$/.test(st.job) ? Number(st.job.slice(5)) : null);

// What the page knows before the device script runs: Android's properties (any adb user may read them).
// -> codes of what stops the page here
export function propBlockers(props) {
  const out = [];
  const model = props["ro.product.model"] || "", device = props["ro.product.device"] || "";
  if (!(/MU5358/i.test(model + " " + device) || /U30\s*Pro/i.test(model))) out.push("not-mu5358");
  if (!/ums9632/i.test(props["ro.board.platform"] || "")) out.push("not-ums9632");
  if ((props["ro.boot.slot_suffix"] || "") !== "_a") out.push("not-slot-a");
  return out;
}

// What stops everything the page does on this device (status from the script, so root is there)
export function deviceBlockers(st) {
  const out = [];
  if (!isMu5358(st)) out.push("not-mu5358");
  if (!/ums9632/i.test(st.platform)) out.push("not-ums9632");
  if (st.slot !== "_a") out.push("not-slot-a");
  if (!st.magiskboot) out.push("no-magiskboot");
  return out;
}

// What stops an installation in particular
export function installBlockers(st) {
  const out = deviceBlockers(st);
  if (jobRunning(st)) out.push("job-running");
  if (st.misc !== "android") out.push("misc-" + st.misc);
  if (st.free_data !== null && st.free_data < DATA_NEED) out.push("no-data-space");
  return out;
}

// The targets and whether each can be chosen: { region: { ok, why, sizes: [{ mib, ok }], fixed }, fulldumpdb: { ok, why } }
export function targets(st) {
  const fd = { ok: true, why: "" };
  if (st.fulldumpdb === "absent") Object.assign(fd, { ok: false, why: "fd-absent" });
  else if (st.fulldumpdb === "used") Object.assign(fd, { ok: false, why: "fd-used" });
  else if (st.fulldumpdb_size && st.fulldumpdb_size < (ROOT_MIB + 1) * MIB) Object.assign(fd, { ok: false, why: "fd-small" });
  const rg = { ok: true, why: "", sizes: [], fixed: 0 };
  if (!st.region) Object.assign(rg, { ok: false, why: "region-unsupported" });
  else if (st.region.state === "ours" || st.region.state === "free") {
    rg.fixed = Math.round((st.region.size || 0) / MIB);
    rg.sizes = [{ mib: rg.fixed, ok: rg.fixed > 0 }];
    if (!rg.fixed) Object.assign(rg, { ok: false, why: "region-unknown" });
  } else {
    // the shrink takes about the region's size from /data's free space, which must keep ANDROID_KEEPS
    const free = st.free_data;
    rg.sizes = REGION_SIZES.map((mib) => ({ mib, ok: free === null || free - mib * MIB >= ANDROID_KEEPS }));
    if (!rg.sizes.some((s) => s.ok)) Object.assign(rg, { ok: false, why: "region-no-room" });
  }
  // only one system: boot/init looks in fulldumpdb first, so a region needs fulldumpdb empty (or ours, erased then)
  if (rg.ok && (st.fulldumpdb === "used")) Object.assign(rg, { ok: false, why: "fd-used" });
  return { region: rg, fulldumpdb: fd };
}

// the target proposed first: where the system already is, else the region, else fulldumpdb
export function defaultTarget(st) {
  const t = targets(st);
  if (st.root && t[st.root] && t[st.root].ok) return st.root;
  if (t.region.ok) return "region";
  if (t.fulldumpdb.ok) return "fulldumpdb";
  return "";
}

// The root password the user types for the system: the one for SSH and the web panel (OpenWrt's root has none
// until one is set, so the installation always sets one). The same rules as MU300 Linux's installer.
export function validPassword(p) {
  if (typeof p !== "string" || p.length < 6 || p.length > 128) return false;
  if (/[\u0000-\u001f\u007f]/.test(p)) return false;           // one line, nothing a terminal would act on
  if (p !== p.trim()) return false;                            // no blank at either end
  if (/^(".*"|'.*')$/.test(p)) return false;                   // not wrapped in a pair of quotes
  return true;
}
// "" while nothing is typed yet; else the code of what is wrong ("password", "password-differs"), or "" when fine
export function passwordProblem(p, again) {
  if (!validPassword(p)) return "password";
  if (p !== again) return "password-differs";
  return "";
}

// Can Linux be started once? -> codes of what stops it
export function bootBlockers(st) {
  const out = deviceBlockers(st);
  if (!st.installed) out.push("nothing-installed");
  if (jobRunning(st)) out.push("job-running");
  if (st.boot_b !== "linux") out.push("boot-b-" + st.boot_b);
  if (rebootPending(st)) out.push("reboot-pending");
  if (st.misc !== "android") out.push("misc-" + st.misc);
  return out;
}

// A line of the device script -> { type, ... }
export function classifyLine(line) {
  const l = String(line).replace(/\r+$/, "");
  let m;
  if ((m = /^MU5358-STEP (\S+)/.exec(l))) return { type: "step", name: m[1] };
  if ((m = /^MU5358-EXIT (-?\d+)/.exec(l))) return { type: "exit", code: Number(m[1]) };
  if ((m = /^MU5358-ERROR ?(.*)$/.exec(l))) return { type: "error", text: m[1] };
  if ((m = /^MU5358-INFO ?(.*)$/.exec(l))) return { type: "info", text: m[1] };
  if ((m = /^MU5358-TAKEN ?(\S*)/.exec(l))) return { type: "taken", version: m[1] };
  if (/^MU5358-JOB started/.test(l)) return { type: "job" };
  if (/^MU5358-PASSWORD set/.test(l)) return { type: "password" };
  if (/^MU5358-REBOOT-NEEDED/.test(l)) return { type: "reboot-needed" };
  if (/^MU5358-REBOOT/.test(l)) return { type: "reboot" };
  if ((m = /^MU5358-FILE (\S+) (\d+)/.exec(l))) return { type: "file", name: m[1], size: Number(m[2]) };
  if (l.startsWith("MU5358-STATUS ")) return { type: "status" };
  if (l.startsWith("MU5358-")) return { type: "mark", text: l };
  return { type: "text", text: l };
}

export function fmtSize(bytes, lang = "en") {
  const b = Number(bytes) || 0;
  const loc = lang === "tr" ? "tr-TR" : "en-GB";
  const f = (v, d) => v.toLocaleString(loc, { maximumFractionDigits: d });
  if (b >= GIB) return f(b / GIB, b >= 10 * GIB ? 0 : 1) + " GiB";
  if (b >= MIB) return f(b / MIB, b >= 10 * MIB ? 0 : 1) + " MiB";
  if (b >= 1024) return f(b / 1024, 0) + " KiB";
  return b + " B";
}
