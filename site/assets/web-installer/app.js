// The web installer's screen. What the page decides by itself is flow.js; what the device is asked is device.js;
// the device script (installer/mu5358-install.sh) checks everything again and has the last word. This file turns
// their answers into the page, in the page's language (strings.js), and keeps a masked log of every line.
//
// Network: the release list from api.github.com, and this site's own files. Nothing else (the CSP says so too).
// A release's files cannot be read by a page from GitHub (no CORS on its download links): the user downloads them
// and picks them here, and the page checks every one against SHA256SUMS, whose own SHA-256 must be the one GitHub's
// API publishes for the release and which must carry the project's signature (SHA256SUMS.sig, releasekey.js).
// TODO (after v1): the device's own download of the release, as on MU300 Linux (no file picking), once it has been
// measured on this device.
import { AdbConnection } from "./adb/connection.js";
import { AdbError } from "./adb/errors.js";
import { fingerprint } from "./adb/key.js";
import { ADB_FILTERS, findAdbInterface, openUsbTransport, platformOf } from "./adb/usb.js";
import { DeviceError, Mu5358Device, ROOT_COPY, STAGED_SCRIPT } from "./device.js";
import { INSTALL_STEPS, MIB, REGION_DEFAULT, UNINSTALL_STEPS, bootBlockers, defaultTarget, deviceBlockers, fmtSize, installBlockers,
  jobExit, jobRunning, parseSums, passwordProblem, propBlockers, rebootPending, targets } from "./flow.js";
import { F, GithubError, filesOf, loadReleases } from "./github.js";
import { loadKey } from "./keystore.js";
import { maskSerial, pageMasker as masker } from "./privacy.js";
import { RELEASE_KEY, releaseKey, verifySums } from "./releasekey.js";
import { sha256Hex } from "./sha256.js";
import { sha512crypt } from "./sha512crypt.js";
import { STR } from "./strings.js";

const root = document.getElementById("wi-app");
const LANG = (document.documentElement.lang || "en").slice(0, 2);
const L = STR[LANG] || STR.en;
const PARAMS = new URLSearchParams(location.search);
const DEMO = PARAMS.has("demo") ? (PARAMS.get("demo") || "fresh") : "";
const PLATFORM = platformOf();
const ISSUES = "https://github.com/dikeckaan/mu5358-linux/issues";

function t(key, ...args) {
  let s = L[key] !== undefined ? L[key] : STR.en[key] !== undefined ? STR.en[key] : key;
  args.forEach((a, i) => { s = s.split(`{${i + 1}}`).join(String(a)); });
  return s;
}
const has = (key) => L[key] !== undefined || STR.en[key] !== undefined;
const size = (b) => fmtSize(b, LANG);

// h("div", { class: "x", onclick: fn }, child, ...): an element; strings become text nodes, never markup
function h(tag, props, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (v === false || v === null || v === undefined) continue;
    if (k === "class") el.className = v;
    else if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
    else if (k === "value") el.value = v;
    else if (k === "checked" || k === "disabled" || k === "hidden" || k === "open" || k === "selected" || k === "multiple") el[k] = !!v;
    else el.setAttribute(k, v === true ? "" : String(v));
  }
  for (const kid of kids.flat(Infinity)) {
    if (kid === null || kid === undefined || kid === false) continue;
    el.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
  }
  return el;
}
// "text [link text] more": the brackets mark the link
function withLink(textValue, href) {
  const m = /^(.*)\[(.+)\](.*)$/s.exec(textValue);
  if (!m) return [textValue];
  return [m[1], h("a", { href }, m[2]), m[3]];
}
function btn(label, onclick, cls = "", props = {}) {
  const { always, ...rest } = props;
  return h("button", { type: "button", class: "wi-btn " + cls, onclick, disabled: !!S.busy && !always, ...rest }, label);
}
const chip = (label, kind) => h("span", { class: "wi-chip " + (kind || "") }, label);
const callout = (kind, title, ...body) => h("div", { class: "callout " + (kind || "") }, title ? h("strong", {}, title) : null,
  ...body.filter(Boolean).map((b) => (typeof b === "string" ? h("p", {}, b) : b)));
const wait = (label) => h("p", { class: "wi-wait" }, label);

// ---------------------------------------------------------------------------------------------------- state

const S = {
  supported: !!(navigator.usb && window.isSecureContext) || !!DEMO,
  phase: "idle",              // idle | connecting | auth | probing | ready | failed | rebooting | linux
  failure: null,
  usb: null, conn: null, dev: null, lost: false,
  key: null, keyInfo: null, keyFp: "",
  probe: null, rootState: "", status: null, showSerial: false,
  releases: null, relError: null, relTag: "",
  files: new Map(),           // name -> File, as picked
  checks: new Map(),          // name -> { state: "hashing" | "hashed" | "needsig" | "ok" | "bad", why, arg }
  signKey: RELEASE_KEY,       // the key SHA256SUMS.sig must be by: the project's (the demo's own in the demo)
  sigFail: null,              // { why, keyNumber } when the picked SHA256SUMS is not signed by signKey
  verified: null,             // { tag, sumsSha, sums, signedBy } once every file of the release checks out
  hashing: "",
  scriptsFor: "",             // the release whose script is on the device, pushed in this visit
  taken: "",                  // the release the device has taken (take) and not used up yet
  target: "", mib: REGION_DEFAULT, confirm: false,
  pw: { a: "", b: "", show: false },   // root's password for the installed system, typed twice; never logged, never sent
  pwHash: "",                 // its sha512-crypt hash, made when Install is pressed: what the device gets (again after a restart)
  job: null,                  // { kind, done: [], now, exit, error, rebootNeeded, phase, push: { name, sent, total }, detached }
  resume: null,               // the installation to go on with after Android's restart: { args }
  un: { keep: false, confirm: false },
  boot: { dry: null, error: null, confirm: false },
  backup: { files: null, error: null, sums: null, got: {} },
  busy: "",
  notice: null,
  granted: [],
};

const LOG = [];
function logLine(line) {
  const m = masker.mask(String(line));
  LOG.push(m);
  if (LOG.length > 20000) LOG.splice(0, LOG.length - 20000);
  const pre = document.getElementById("wi-log-pre");
  if (pre) {
    pre.append(document.createTextNode(m + "\n"));
    const c = document.getElementById("wi-log-count");
    if (c) c.textContent = String(LOG.length);
    pre.scrollTop = pre.scrollHeight;
  }
}
const note = (text) => logLine("# " + text);

// ---------------------------------------------------------------------------------------------------- errors

function explainError(e) {
  if (e instanceof AdbError) return { code: "adb." + e.code, platform: e.platform || PLATFORM, detail: e.message };
  if (e instanceof GithubError) return { code: "gh." + e.code, detail: e.message };
  if (e && e.name === "DeviceError") return { code: "dev." + e.code, detail: e.detail || "" };
  if (e && e.name === "SecurityError") return { code: "adb.access", platform: PLATFORM, detail: e.message };
  return { code: "generic", detail: masker.mask(String((e && e.message) || e)) };
}
function errorText(x) {
  if (!x) return "";
  if (x.code === "adb.access") return t("err.adb.access." + (["windows", "linux", "mac"].includes(x.platform) ? x.platform : "other"));
  const key = "err." + x.code;
  return has(key) ? t(key, x.detail || "") : t("err.generic", x.code, x.detail || "");
}
const isGone = (e) => e instanceof AdbError && ["disconnected", "closed"].includes(e.code);

async function guarded(label, fn) {
  if (S.busy) return;
  S.busy = label; S.notice = null; render();
  try { await fn(); } catch (e) {
    if (isGone(e) && !S.conn) { /* the connection's own handler has said it */ } else {
      S.notice = { kind: "warn", text: errorText(explainError(e)) };
      note(label + " failed: " + (e && e.message ? e.message : e));
    }
  } finally { S.busy = ""; render(); }
}

// ---------------------------------------------------------------------------------------------------- connecting

async function chooseAndConnect() {
  if (S.busy) return;
  if (DEMO) return demoConnect();
  let usb;
  try { usb = await navigator.usb.requestDevice({ filters: ADB_FILTERS }); } catch (e) {
    if (e && e.name === "NotFoundError") return;
    S.phase = "failed"; S.failure = explainError(e); render(); return;
  }
  await connectTo(usb);
}

async function loadGranted() {
  try { S.granted = (await navigator.usb.getDevices()).filter((d) => findAdbInterface(d)); } catch (e) { S.granted = []; }
}
let reconnectTimer = null;
function reconnectSoon(usb) {
  if (reconnectTimer) clearTimeout(reconnectTimer);
  // just after Android starts it sets its USB functions up a second time: a connection made then is cut again
  reconnectTimer = setTimeout(async () => {
    reconnectTimer = null;
    if (!["idle", "rebooting"].includes(S.phase) || !S.lost || S.busy || !findAdbInterface(usb)) return;
    note("the device is back: connecting");
    await connectTo(usb);
  }, 4000);
}

async function disconnect() {
  const c = S.conn;
  S.conn = null; S.dev = null;
  if (c) { try { await c.close(); } catch (e) { /* gone */ } }
}

function onConnectionLost(c) {
  if (S.conn !== c) return;
  S.conn = null; S.dev = null; S.lost = true;
  if (S.job && S.job.phase === "follow" && S.job.exit === null) S.job.detached = true;
  if (S.phase !== "rebooting" && S.phase !== "linux") S.phase = "idle";
  note("the connection ended");
  render();
}

async function connectTo(usb) {
  await disconnect();
  S.usb = usb; S.phase = "connecting"; S.failure = null;
  if (usb.serialNumber) masker.add(usb.serialNumber);
  render();
  try {
    await ensureKey();
    if (!findAdbInterface(usb)) throw new AdbError("no-interface");
    let conn = null;
    for (let attempt = 1; ; attempt++) {
      const transport = await openUsbTransport(usb, PLATFORM);
      let up = false, silent = false;
      const c = new AdbConnection(transport, { key: S.key,
        onAuth: (state) => { if (state === "public-key") { S.phase = "auth"; render(); } },
        onClose: () => { if (up) onConnectionLost(c); } });
      const quiet = setTimeout(() => { if (S.usb === usb && S.phase === "connecting") { silent = true; c.close(); } }, 6000);
      try {
        await c.connect("mu5358-web");
        up = true; conn = c;
        break;
      } catch (e) {
        const err = silent ? new AdbError("silent", "the device did not answer") : e;
        try { await c.close(); } catch (e2) { /* gone */ }
        if (S.usb !== usb) return;
        const again = attempt < 3 && S.phase === "connecting" && err instanceof AdbError && ["silent", "disconnected", "protocol"].includes(err.code);
        if (!again) throw err;
        note(`the device did not take the connection (${err.code}); trying again`);
        await new Promise((r) => setTimeout(r, 1500));
      } finally { clearTimeout(quiet); }
    }
    S.conn = conn;
    note(`connected: ${usb.manufacturerName || ""} ${usb.productName || ""} (${maskSerial(usb.serialNumber || "")})`);
    await afterConnect(new Mu5358Device(conn, { log: logLine }));
  } catch (e) {
    if (S.usb !== usb) return;
    S.phase = "failed"; S.failure = explainError(e);
    note("connect failed: " + S.failure.code + " " + (S.failure.detail || ""));
    await disconnect();
    render();
  }
}

async function ensureKey() {
  if (S.key) return;
  S.keyInfo = await loadKey("mu5358-web");
  S.key = S.keyInfo.key;
  S.keyFp = await fingerprint(S.key);
}

async function afterConnect(dev) {
  S.dev = dev; S.lost = false;
  S.phase = "probing"; S.status = null; S.rootState = ""; S.scriptsFor = ""; S.boot = { dry: null, error: null, confirm: false };
  S.backup = { files: null, error: null, sums: null, got: {} };
  render();
  try {
    S.probe = await dev.probe();
    note(`device: ${S.probe.props["ro.product.model"] || "?"} / ${S.probe.props["ro.product.device"] || "?"} (${S.probe.props["ro.board.platform"] || "?"}), slot ${S.probe.props["ro.boot.slot_suffix"] || "?"}`);
    S.phase = "ready";
    if (propBlockers(S.probe.props).length) { render(); return; }
    S.rootState = S.probe.su ? "asking" : "no-su";
    render();
    if (!S.probe.su) return;
    S.rootState = await dev.root(90000);
    note("root: " + S.rootState);
    render();
    if (S.rootState !== "granted") return;
    if (S.verified) await pushScripts();
    else await dev.findRootCopy();
    if (dev.script) await refreshStatus();
    await afterStatus();
  } catch (e) {
    if (isGone(e)) return;
    S.phase = "failed"; S.failure = explainError(e); render();
  }
}

// what a fresh status asks for: a job to follow, an installation to go on with
async function afterStatus() {
  const st = S.status;
  if (!st || !S.dev) return;
  if (jobRunning(st)) { S.job = newJob("", "follow"); render(); await followJob(); return; }
  if (S.resume && !rebootPending(st) && S.verified && deviceBlockers(st).length === 0) {
    const args = S.resume.args;
    S.resume = null;
    note("Android has restarted: the installation goes on");
    await startInstall(args);
  }
}

async function refreshStatus() {
  if (!S.dev || !S.dev.script) return;
  try {
    S.status = await S.dev.status();
    if (!S.target || !targets(S.status)[S.target] || !targets(S.status)[S.target].ok) S.target = defaultTarget(S.status);
  } catch (e) {
    if (isGone(e)) return;
    S.status = null;
    S.notice = { kind: "warn", text: errorText(explainError(e)) };
  }
  render();
}

async function pushScripts() {
  if (!S.dev || !S.verified || S.rootState !== "granted") return;
  if (S.scriptsFor === S.verified.tag && S.dev.script === STAGED_SCRIPT) return;
  await S.dev.pushScripts(S.files);
  S.scriptsFor = S.verified.tag;
  note(`pushed ${F.script} and ${F.list} of ${S.verified.tag}`);
}

// ---------------------------------------------------------------------------------------------------- the release

async function loadReleaseList() {
  try {
    if (DEMO) {
      const demo = await import("./demo.js");
      S.signKey = releaseKey(demo.DEMO_PUB);
      S.releases = [(await demo.demoRelease(DEMO)).release];
    } else S.releases = await loadReleases();
    if (S.releases.length && !S.relTag) S.relTag = S.releases[0].tag;
  } catch (e) { S.relError = explainError(e); S.releases = []; }
  render();
}
const release = () => (S.releases || []).find((r) => r.tag === S.relTag) || null;

function chooseRelease(tag) {
  S.relTag = tag;
  S.files = new Map(); S.checks = new Map(); S.verified = null; S.sigFail = null; S.scriptsFor = ""; S.taken = "";
  render();
}

// "mu5358-openwrt.img (1).gz" (a second download of the same file) -> "mu5358-openwrt.img.gz"
const plainName = (n) => n.replace(/ \(\d+\)(?=\.[A-Za-z0-9]+$)/, "").replace(/ \(\d+\)$/, "");

async function pickFiles(list) {
  const rel = release();
  if (!rel || S.hashing) return;
  const wanted = filesOf(rel);
  const ignored = [];
  for (const f of list) {
    const n = plainName(f.name);
    if (wanted.includes(n)) { S.files.set(n, f); S.checks.delete(n); } else ignored.push(f.name);
  }
  if (ignored.length) note("not a file of this release, left out: " + ignored.join(", "));
  await verifyFiles();
}

// Every picked file is hashed once. SHA256SUMS counts when it has GitHub's digest for the release, names the
// release, AND SHA256SUMS.sig is the project's key's signature of its bytes (releasekey.js); every other file must
// then have the SHA-256 SHA256SUMS names. Without the signature nothing is verified, whatever GitHub's digest says.
async function verifyFiles() {
  const rel = release();
  S.verified = null; S.sigFail = null;
  const sumsAsset = rel.assets.get(F.sums);
  let sums = null, sumsSha = "", signedBy = "";
  for (const name of filesOf(rel)) {
    const f = S.files.get(name);
    if (!f || S.checks.has(name)) continue;
    S.hashing = name; S.checks.set(name, { state: "hashing" }); render();
    let sha = "";
    try { sha = await sha256Hex(new Uint8Array(await f.arrayBuffer())); } catch (e) { S.checks.set(name, { state: "bad", why: "unreadable" }); continue; }
    S.checks.set(name, { state: "hashed", sha });
  }
  S.hashing = "";
  // SHA256SUMS first: GitHub's digest, the release's tag, then the signature; everything else is checked against it
  const sc = S.checks.get(F.sums), sigc = S.checks.get(F.sig);
  if (sc && sc.sha) {
    if (sc.sha !== sumsAsset.sha256) S.checks.set(F.sums, { state: "bad", why: "sums-not-github", sha: sc.sha });
    else {
      const sumsFile = S.files.get(F.sums);
      sums = parseSums(await sumsFile.text());
      if (sums.version !== rel.tag) S.checks.set(F.sums, { state: "bad", why: "sums-version", sha: sc.sha });
      else if (!sigc || !sigc.sha) S.checks.set(F.sums, { state: "needsig", sha: sc.sha });
      else {
        const r = await verifySums(new Uint8Array(await sumsFile.arrayBuffer()), await S.files.get(F.sig).text(), S.signKey);
        if (r.ok) {
          sumsSha = sc.sha; signedBy = r.keyNumber;
          S.checks.set(F.sums, { state: "ok", sha: sc.sha });
          S.checks.set(F.sig, { state: "ok", sha: sigc.sha, arg: r.keyNumber.slice(0, 8) });
          note(`${F.sig}: ${F.sums} is signed by key ${r.keyNumber} (checked with ${r.how === "js" ? "the page's own Ed25519 code" : "WebCrypto"})`);
        } else {
          S.sigFail = { why: r.why, keyNumber: r.keyNumber };
          S.checks.set(F.sums, { state: "bad", why: "sums-unsigned", sha: sc.sha });
          S.checks.set(F.sig, { state: "bad", why: r.why, sha: sigc.sha, arg: r.keyNumber.slice(0, 8) });
          note(`${F.sig}: refused (${r.why}${r.keyNumber ? ", key " + r.keyNumber : ""}); the page wants key ${S.signKey.number}`);
        }
      }
    }
  }
  const sumsOk = !!sumsSha;
  let all = sumsOk;
  for (const name of filesOf(rel)) {
    if (name === F.sums || name === F.sig) continue;
    const c = S.checks.get(name);
    if (!c) { all = false; continue; }
    if (!c.sha) { all = false; continue; }
    if (!sumsOk) { S.checks.set(name, { state: "hashed", sha: c.sha }); all = false; continue; }
    const asset = rel.assets.get(name), f = S.files.get(name);
    let why = "";
    if (!sums.files.has(name)) why = "not-in-sums";
    else if (sums.files.get(name) !== c.sha) why = "sha-differs";
    else if (asset && asset.size !== f.size) why = "size-differs";
    else if (asset && asset.sha256 && asset.sha256 !== c.sha) why = "sha-differs";
    S.checks.set(name, why ? { state: "bad", why, sha: c.sha } : { state: "ok", sha: c.sha });
    if (why) all = false;
  }
  if (all) {
    S.verified = { tag: rel.tag, sumsSha, sums, signedBy };
    note(`the files of ${rel.tag} check out against SHA256SUMS (${sumsSha.slice(0, 16)}…, GitHub's digest, signed by key ${signedBy})`);
  }
  render();
  if (S.verified && S.dev && S.rootState === "granted") {
    await guarded("push-scripts", async () => { await pushScripts(); await refreshStatus(); await afterStatus(); });
  }
}

// ---------------------------------------------------------------------------------------------------- jobs

function newJob(kind, phase) {
  return { kind, done: [], now: "", exit: null, error: "", rebootNeeded: false, phase, push: null, detached: false };
}

async function install() {
  const st = S.status;
  if (!st || !S.verified || !S.target) return;
  const t0 = targets(st)[S.target];
  if (!t0.ok) return;
  const args = S.target === "fulldumpdb" ? ["fulldumpdb"] : t0.fixed ? ["region"] : ["region", String(S.mib)];
  if (passwordProblem(S.pw.a, S.pw.b) || S.busy) return;
  S.confirm = false;
  // the password's hash is made here, once: the device gets the hash and never the password (sha512crypt.js); the
  // password itself is masked out of the log from now on, should it ever appear there
  S.busy = "install"; S.notice = null;
  S.job = newJob("install", "hash"); S.job.args = args;
  render();
  try { S.pwHash = await sha512crypt(S.pw.a); } catch (e) {
    S.job.phase = "end"; S.job.error = errorText(explainError(e)); S.busy = ""; render(); return;
  }
  masker.add(S.pw.a, "***");
  S.busy = "";
  await startInstall(args);
}

// the installation, from the files on the device: also what goes on after Android's restart (afterStatus), with
// the hash made when Install was pressed - the device wants the password before every run install
async function startInstall(args) {
  if (S.busy) return;
  S.busy = "install"; S.notice = null;
  S.job = newJob("install", "push");
  S.job.args = args;
  render();
  try {
    await pushScripts();
    if (S.taken !== S.verified.tag) {
      await S.dev.pushRelease(S.files, (name, sent, total) => {
        S.job.push = { name, sent, total };
        const bar = document.getElementById("wi-push-bar");
        if (bar) { bar.style.width = (100 * sent / total).toFixed(1) + "%"; document.getElementById("wi-push-text").textContent = t("job.pushing", name, size(sent), size(total)); } else render();
      });
      S.job.phase = "take"; render();
      await S.dev.take(S.verified.sumsSha);
      S.taken = S.verified.tag;
    }
    if (!S.pwHash) throw new DeviceError("no-password");
    S.job.phase = "password"; render();
    await S.dev.password(S.pwHash);
    S.job.phase = "start"; render();
    await S.dev.run("install", ...args);
  } catch (e) {
    S.job.phase = "end"; S.job.error = errorText(explainError(e)); S.busy = "";
    if (!isGone(e)) await refreshStatus();
    render();
    return;
  }
  S.busy = "";
  await followJob();
}

async function uninstall() {
  if (S.busy || !S.verified) return;
  S.busy = "uninstall"; S.notice = null;
  S.job = newJob("uninstall", "start");
  S.un.confirm = false;
  render();
  try {
    await pushScripts();
    await S.dev.run("uninstall", ...(S.un.keep ? ["keep-space"] : []));
  } catch (e) {
    S.job.phase = "end"; S.job.error = errorText(explainError(e)); S.busy = "";
    render();
    return;
  }
  S.busy = "";
  await followJob();
}

async function followJob() {
  const j = S.job;
  if (!S.dev || !j) return;
  j.phase = "follow"; j.detached = false; j.done = []; j.now = ""; j.exit = null; j.error = ""; j.rebootNeeded = false;
  S.busy = "follow"; render();
  try {
    const exit = await S.dev.follow((ev) => {
      if (ev.type === "step") {
        if (!j.kind) j.kind = UNINSTALL_STEPS.includes(ev.name) && ev.name !== "done" ? "uninstall" : "install";
        if (j.now && !j.done.includes(j.now)) j.done.push(j.now);
        j.now = ev.name;
        if (ev.name === "done") { j.done.push("done"); j.now = ""; }
        render();
      } else if (ev.type === "error") { j.error = ev.text; render(); }
      else if (ev.type === "reboot-needed") { j.rebootNeeded = true; render(); }
    });
    j.exit = exit; j.phase = "end";
    if (j.kind === "install" && exit === 3 && j.rebootNeeded) {
      S.resume = { args: j.args || ["region", String(S.mib)] };
      j.error = "";
    } else if (j.kind === "install" && exit === 0) {
      S.taken = "";
      S.pw = { a: "", b: "", show: false }; S.pwHash = "";          // set on the device now; the page keeps nothing
    }
  } catch (e) {
    if (isGone(e) || !S.conn) { j.detached = true; S.busy = ""; render(); return; }
    j.phase = "end"; j.error = errorText(explainError(e));
  }
  S.busy = "";
  await refreshStatus();
}

// Android's restart after a shrink: the page waits for the device to come back and goes on by itself
async function restartAndroid() {
  if (S.busy || !S.dev) return;
  S.busy = "reboot"; S.phase = "rebooting"; render();
  note("restarting Android");
  try { await S.dev.reboot(); } catch (e) { /* the connection goes with the restart */ }
  S.busy = "";
  if (DEMO) {
    const dev = S.dev;
    S.conn = null; S.dev = null; S.lost = true; render();
    setTimeout(() => demoConnect(dev), 2500);
    return;
  }
  render();
}

// ---------------------------------------------------------------------------------------------------- start Linux once, backup

async function bootCheck() {
  await guarded("boot-check", async () => {
    S.boot = { dry: null, error: null, confirm: false };
    try { S.boot.dry = await S.dev.bootLinux(true); } catch (e) { if (isGone(e)) throw e; S.boot.error = errorText(explainError(e)); }
  });
}
async function bootLinux() {
  await guarded("boot-linux", async () => {
    await S.dev.bootLinux(false);
    note("the device restarts into Linux, once");
    S.phase = "linux";
    if (DEMO) { S.conn = null; S.dev = null; }
  });
}

async function listBackup() {
  await guarded("backup", async () => {
    S.backup = { files: null, error: null, sums: null, got: {} };
    try { S.backup.files = await S.dev.backupList(); } catch (e) { if (isGone(e)) throw e; S.backup.error = errorText(explainError(e)); }
  });
}
function saveBlob(bytes, name, type = "application/octet-stream") {
  const blob = bytes instanceof Blob ? bytes : new Blob([bytes], { type });
  const a = h("a", { href: URL.createObjectURL(blob), download: name });
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 60000);
}
async function downloadBackup(name) {
  await guarded("backup-" + name, async () => {
    if (!S.backup.sums) S.backup.sums = parseSums(new TextDecoder().decode(await S.dev.cat("SHA256SUMS")));
    const bytes = await S.dev.cat(name);
    let ok = "unchecked";
    if (name !== "SHA256SUMS" && S.backup.sums.files.has(name)) ok = (await sha256Hex(bytes)) === S.backup.sums.files.get(name) ? "ok" : "bad";
    S.backup.got[name] = ok;
    note(`backup ${name}: ${bytes.length} bytes, ${ok === "ok" ? "matches the device's SHA256SUMS" : ok}`);
    if (ok === "bad") throw Object.assign(new Error(name), { name: "DeviceError", code: "backup-differs", detail: name });
    const serial = S.status ? S.status.serial.slice(-4) : "";
    saveBlob(bytes, `mu5358-backup${serial ? "-" + serial : ""}-${name}`);
  });
}

function saveLog() {
  const head = [`# MU5358 Linux web installer log, ${new Date().toISOString()}`, `# page: ${location.pathname}${DEMO ? " (demo)" : ""}`,
    `# browser: ${navigator.userAgent}`, "# serial numbers, identity numbers, MAC addresses and keys are masked", ""];
  saveBlob(head.concat(LOG).join("\n") + "\n", `mu5358-web-installer-${new Date().toISOString().replace(/[:.]/g, "-")}.log`, "text/plain");
}

// ---------------------------------------------------------------------------------------------------- the screen

function facts(rows) {
  return h("dl", { class: "wi-facts" }, rows.filter(Boolean).map(([k, v]) => [h("dt", {}, k), h("dd", {}, v)]));
}

function blockerList(codes) {
  return h("ul", { class: "wi-check" }, codes.map((c) => h("li", {}, t("block." + c))));
}

function panelDevice() {
  const p = h("section", { class: "panel wi-panel", "aria-labelledby": "wi-dev-h" });
  const head = h("div", { class: "wi-device-head" }, h("h2", { id: "wi-dev-h" }, t("dev.title")));
  p.append(head);
  if (DEMO) p.append(callout("linux", t("demo.title"), t("demo.text")));
  if (!S.supported) { p.append(callout("warn", t("nousb.title"), t("nousb.text"))); return p; }
  const ph = S.phase;
  if (ph === "idle" || ph === "failed") {
    if (S.lost) p.append(callout("", t("dev.lost.title"), t("dev.lost.text")));
    if (ph === "failed") p.append(callout("warn", t("err.title"), errorText(S.failure)));
    p.append(h("p", {}, t("dev.intro")));
    p.append(h("div", { class: "actions" }, btn(t("dev.connect"), chooseAndConnect, "primary", { id: "wi-connect" }),
      ...S.granted.filter(() => !DEMO).map((d) => btn(t("dev.again", d.productName || "adb"), () => connectTo(d), "")),
    ));
    return p;
  }
  if (ph === "connecting") { p.append(wait(t("dev.connecting"))); return p; }
  if (ph === "auth") { p.append(callout("", t("dev.auth.title"), t("dev.auth.text"))); p.append(wait(t("dev.auth.wait"))); return p; }
  if (ph === "rebooting") { p.append(callout("", t("dev.rebooting.title"), t("dev.rebooting.text"))); p.append(wait(t("dev.rebooting.wait")));
    if (!DEMO) p.append(h("div", { class: "actions" }, btn(t("dev.connect"), chooseAndConnect, "", { always: true })));
    return p; }
  if (ph === "linux") { p.append(callout("linux", t("boot.gone.title"), t("boot.gone.text"))); return p; }
  if (ph === "probing" && !S.probe) { p.append(wait(t("dev.probing"))); return p; }

  head.append(btn(t("dev.disconnect"), async () => { await disconnect(); S.phase = "idle"; S.status = null; S.probe = null; S.lost = false; render(); }, "small quiet", { always: true }));
  const st = S.status, pr = S.probe ? S.probe.props : {};
  const serial = st ? st.serial : (S.usb && S.usb.serialNumber) || "";
  const serialCell = serial ? h("span", {}, h("span", { class: "wi-mono" }, S.showSerial ? serial : maskSerial(serial)), " ",
    btn(S.showSerial ? t("dev.serial.hide") : t("dev.serial.show"), () => { S.showSerial = !S.showSerial; render(); }, "link", { always: true })) : "–";
  const rootCell = S.rootState === "granted" ? chip(st && st.magisk ? t("dev.root.yes", st.magisk.split(":")[0]) : t("dev.root.granted"), "ok")
    : S.rootState === "asking" ? chip(t("dev.root.asking"), "wait") : S.rootState ? chip(t("dev.root." + S.rootState), "bad") : "–";
  const rows = [
    [t("dev.model"), `${(st && st.model) || pr["ro.product.model"] || "?"} (${(st && st.device) || pr["ro.product.device"] || "?"}, ${(st && st.platform) || pr["ro.board.platform"] || "?"})`],
    [t("dev.android"), `${(st && st.android) || pr["ro.build.version.release"] || "?"} · ${(st && st.build) || pr["ro.build.display.id"] || "?"}`],
    [t("dev.serial"), serialCell],
    [t("dev.slot"), ((st && st.slot) || pr["ro.boot.slot_suffix"] || "?") === "_a" ? chip(t("dev.slot.a"), "ok") : chip(t("dev.slot.other", (st && st.slot) || pr["ro.boot.slot_suffix"] || "?"), "bad")],
    [t("dev.root"), rootCell],
  ];
  if (st) {
    rows.push([t("dev.magiskboot"), st.magiskboot ? chip(t("yes"), "ok") : chip(t("no"), "bad")]);
    rows.push([t("dev.misc"), chip(t("misc." + st.misc), st.misc === "android" ? "android" : st.misc === "trial" ? "linux" : "bad")]);
    rows.push([t("dev.bootb"), chip(t("bootb." + st.boot_b), st.boot_b === "linux" ? "linux" : st.boot_b === "other" ? "warn" : "")]);
    rows.push([t("dev.fd"), [chip(t("fd." + st.fulldumpdb), st.fulldumpdb === "ours" ? "linux" : st.fulldumpdb === "used" ? "warn" : ""), " ", st.fulldumpdb_size ? size(st.fulldumpdb_size) : ""]]);
    if (st.region) rows.push([t("dev.region"), [chip(t("region." + st.region.state), st.region.state === "ours" ? "linux" : ""), " ", st.region.size ? size(st.region.size) : ""]]);
    if (st.userdata && st.userdata.size) rows.push([t("dev.userdata"), [size(st.userdata.size), rebootPending(st) ? [" ", chip(t("dev.userdata.pending", size(st.userdata.kernel_size)), "warn")] : null]]);
    if (st.free_data !== null) rows.push([t("dev.free"), size(st.free_data)]);
    rows.push([t("dev.installed"), st.installed ? [chip(st.installed, "linux"), " ", st.root ? t("where." + st.root) : ""] : t("dev.installed.none")]);
    rows.push([t("dev.backup"), st.backup ? t("yes") : t("no")]);
    rows.push([t("dev.job"), st.job === "none" ? t("job.none") : st.job === "running" ? chip(t("job.running"), "wait") : t("job.exited", jobExit(st))]);
  }
  p.append(facts(rows));
  if (S.probe) {
    const pb = propBlockers(S.probe.props);
    if (pb.length) { p.append(callout("warn", t("block.title"), blockerList(pb), t("block.nothing"))); return p; }
  }
  if (S.rootState === "asking") p.append(callout("", t("root.ask.title"), t("root.ask.text")));
  if (S.rootState === "denied" || S.rootState === "no-su") {
    p.append(callout("warn", t("root." + S.rootState + ".title"), t("root." + S.rootState + ".text")));
    p.append(h("div", { class: "actions" }, btn(t("root.again"), () => guarded("root", async () => { S.rootState = "asking"; render(); S.rootState = await S.dev.root(90000); if (S.rootState === "granted") { if (S.verified) await pushScripts(); else await S.dev.findRootCopy(); await refreshStatus(); await afterStatus(); } }), "")));
  }
  if (st) {
    const db = deviceBlockers(st);
    if (db.length) p.append(callout("warn", t("block.title"), blockerList(db), t("block.nothing")));
    else if (rebootPending(st) && !(S.job && S.job.phase === "follow")) {
      p.append(callout("warn", t("pending.title"), t("pending.text"), h("div", { class: "actions" }, btn(t("pending.button"), restartAndroid, "primary"))));
    }
    p.append(h("div", { class: "actions" }, btn(t("dev.refresh"), () => guarded("status", refreshStatus), "small")));
  } else if (S.rootState === "granted" && S.dev && !S.dev.script) {
    p.append(callout("", t("dev.nostatus.title"), t("dev.nostatus.text")));
  } else if (S.rootState === "granted") p.append(wait(t("dev.status.wait")));
  if (st && S.dev && S.dev.script === ROOT_COPY) p.append(h("p", { class: "small muted" }, t("dev.rootcopy")));
  return p;
}

function panelRelease() {
  const p = h("section", { class: "panel wi-panel", "aria-labelledby": "wi-rel-h" }, h("h2", { id: "wi-rel-h" }, t("rel.title")));
  if (S.releases === null) { p.append(wait(t("rel.loading"))); return p; }
  if (S.relError) p.append(callout("warn", t("rel.error.title"), errorText(S.relError)));
  if (!S.releases.length) { if (!S.relError) p.append(callout("", t("rel.none.title"), t("rel.none.text"))); return p; }
  const rel = release();
  p.append(h("label", { class: "wi-field" }, t("rel.choose"),
    h("select", { onchange: (ev) => chooseRelease(ev.target.value), disabled: !!S.busy },
      S.releases.map((r, i) => h("option", { value: r.tag, selected: r.tag === S.relTag }, r.tag + (r.prerelease ? " (" + t("rel.beta") + ")" : "") + (i === 0 ? " – " + t("rel.newest") : ""))))));
  if (!rel) return p;
  p.append(h("p", {}, ...withLink(t("rel.download"), rel.page)));
  const list = h("ul", { class: "wi-filelist" });
  for (const n of filesOf(rel)) {
    const a = rel.assets.get(n), c = S.checks.get(n);
    const state = !S.files.has(n) ? chip(t("file.missing"), "") : !c ? chip(t("file.waiting"), "wait") : c.state === "hashing" ? chip(t("file.hashing"), "wait")
      : c.state === "ok" ? chip(n === F.sig ? t("file.sig.ok", c.arg) : t("file.ok"), "ok") : c.state === "hashed" ? chip(t("file.hashed"), "wait")
      : c.state === "needsig" ? chip(t("file.needsig"), "wait") : chip(t("file.bad." + c.why, c.arg || ""), "bad");
    list.append(h("li", {}, state, " ", h("a", { href: a.url, rel: "noopener" }, n), " ", h("span", { class: "small muted" }, size(a.size))));
  }
  p.append(list);
  const input = h("input", { type: "file", multiple: true, id: "wi-files", class: "wi-hidden", onchange: (ev) => { const fl = [...ev.target.files]; ev.target.value = ""; pickFiles(fl); } });
  p.append(input);
  const acts = h("div", { class: "actions" }, h("label", { class: "wi-btn" + (S.verified ? "" : " primary"), for: "wi-files" }, t("rel.pick")));
  if (DEMO) acts.append(btn(t("demo.files"), async () => { const d = await import("./demo.js"); pickFiles((await d.demoRelease(DEMO)).files); }, "small", { id: "wi-demo-files" }));
  p.append(acts);
  if (S.hashing) p.append(wait(t("file.hashing.now", S.hashing)));
  if (S.sigFail) {
    p.append(callout("warn", t("sig.failed.title"), t("sig.failed." + S.sigFail.why, S.sigFail.keyNumber.slice(0, 8), S.signKey.short), t("sig.failed.text")));
  }
  if (S.verified) p.append(h("p", { class: "wi-good", id: "wi-verified" }, t("rel.verified", S.verified.tag, S.verified.signedBy.slice(0, 8))));
  else p.append(h("p", { class: "small muted" }, t("rel.why")));
  p.append(h("p", { class: "small muted", id: "wi-release-key" }, t(DEMO ? "rel.signed.demo" : "rel.signed", S.signKey.short), " ", h("code", {}, S.signKey.number)));
  return p;
}

function panelInstall() {
  const st = S.status;
  if (!st || deviceBlockers(st).length) return null;
  const p = h("section", { class: "panel wi-panel", "aria-labelledby": "wi-inst-h" }, h("h2", { id: "wi-inst-h" }, st.installed ? t("inst.title.again") : t("inst.title")));
  const blocks = installBlockers(st).filter((c) => !(c === "job-running" && S.job));
  if (blocks.length) { p.append(callout("warn", t("inst.blocked"), blockerList(blocks))); return p; }
  if (rebootPending(st)) { p.append(h("p", {}, t("pending.inst"))); return p; }
  const tg = targets(st);
  const opt = (value, title, body, ok, why, extra) => h("label", { class: "wi-opt" + (ok ? "" : " off") },
    h("input", { type: "radio", name: "wi-target", value, checked: S.target === value, disabled: !ok || !!S.busy, onchange: () => { S.target = value; render(); } }),
    h("span", {}, h("strong", {}, title), h("br"), h("span", { class: "small" }, body), !ok && why ? [h("br"), h("span", { class: "small wi-bad" }, t("why." + why))] : null, extra || null));
  let sizes = null;
  if (tg.region.ok && S.target === "region") {
    if (tg.region.fixed) sizes = h("p", { class: "small" }, t("inst.region.fixed", size(tg.region.fixed * MIB), t("region." + st.region.state)));
    else {
      if (!tg.region.sizes.find((s) => s.mib === S.mib && s.ok)) S.mib = (tg.region.sizes.find((s) => s.ok) || { mib: REGION_DEFAULT }).mib;
      sizes = h("div", { class: "wi-field" }, h("span", { class: "small" }, t("inst.region.size")),
        h("div", { class: "actions" }, tg.region.sizes.map((s) => h("label", { class: "wi-opt" + (s.ok ? "" : " off") },
          h("input", { type: "radio", name: "wi-mib", checked: S.mib === s.mib, disabled: !s.ok || !!S.busy, onchange: () => { S.mib = s.mib; render(); } }),
          h("span", {}, size(s.mib * MIB), s.mib === REGION_DEFAULT ? " (" + t("inst.default") + ")" : "")))),
        h("span", { class: "small" }, t("inst.region.shrink", size(st.free_data || 0))));
    }
  }
  p.append(h("div", { class: "wi-opts" },
    opt("region", t("inst.region"), t("inst.region.text"), tg.region.ok, tg.region.why, sizes),
    opt("fulldumpdb", t("inst.fd"), t("inst.fd.text"), tg.fulldumpdb.ok, tg.fulldumpdb.why)));
  if (st.boot_b === "other") p.append(callout("warn", t("inst.bootb.other.title"), t("inst.bootb.other.text")));
  p.append(h("p", {}, t("inst.writes")));
  p.append(h("ul", {}, [t("inst.w.backup"), S.target === "region" && !tg.region.fixed ? t("inst.w.shrink") : null, t("inst.w.chain"), t("inst.w.system." + (S.target || "region")), t("inst.w.boot")].filter(Boolean).map((x) => h("li", {}, x))));
  p.append(h("p", { class: "small" }, t("inst.never")));
  // root's password: typed twice, checked as it is typed (pwSync: the hint and the button, without drawing the
  // panel again, so the field keeps the cursor)
  p.append(h("h3", {}, t("inst.pw.title")), h("p", { class: "small" }, t("inst.pw.text")), h("p", { class: "small muted" }, t("inst.pw.rules")));
  p.append(h("div", { class: "wi-pw" },
    h("label", { class: "wi-field" }, t("inst.pw.label"), pwInput("a", "wi-pw1")),
    h("label", { class: "wi-field" }, t("inst.pw.again"), pwInput("b", "wi-pw2"))));
  p.append(h("label", { class: "wi-show" }, h("input", { type: "checkbox", id: "wi-pw-show", checked: S.pw.show, onchange: (ev) => {
    S.pw.show = ev.target.checked;
    for (const el of document.querySelectorAll("input[data-pw]")) el.type = S.pw.show ? "text" : "password";
  } }), t("inst.pw.show")));
  p.append(h("p", { class: "small wi-bad", id: "wi-pw-hint" }, pwHint()));
  if (!S.verified) { p.append(callout("", t("inst.needfiles.title"), t("inst.needfiles.text"))); return p; }
  p.append(h("label", { class: "wi-opt" }, h("input", { type: "checkbox", checked: S.confirm, disabled: !!S.busy, onchange: (ev) => { S.confirm = ev.target.checked; render(); } }), h("span", {}, t("inst.confirm"))));
  p.append(h("div", { class: "actions" }, btn(t("inst.button", S.verified.tag), install, "primary", { id: "wi-install", disabled: !!S.busy || !S.confirm || !S.target || !tg[S.target].ok || !!passwordProblem(S.pw.a, S.pw.b) })));
  return p;
}

function pwInput(key, id) {
  return h("input", { type: S.pw.show ? "text" : "password", autocomplete: "new-password", spellcheck: "false", id, "data-pw": key, value: S.pw[key],
    disabled: !!S.busy, oninput: (ev) => { S.pw[key] = ev.target.value; pwSync(); } });
}
// what is wrong with the password as typed so far, said only once there is something to say
function pwHint() {
  const prob = passwordProblem(S.pw.a, S.pw.b);
  if (!prob || !S.pw.a) return "";
  if (prob === "password-differs" && !S.pw.b) return "";
  return t("verr." + prob);
}
function pwSync() {
  const hint = document.getElementById("wi-pw-hint");
  if (hint) hint.textContent = pwHint();
  const b = document.getElementById("wi-install");
  if (b) b.disabled = !!S.busy || !S.confirm || !S.target || !targets(S.status)[S.target].ok || !!passwordProblem(S.pw.a, S.pw.b);
}

function panelJob() {
  const j = S.job;
  if (!j) return null;
  const kind = j.kind || "install";
  const p = h("section", { class: "panel wi-panel", "aria-labelledby": "wi-job-h", "aria-live": "polite" }, h("h2", { id: "wi-job-h" }, t("job.title." + kind)));
  if (j.phase === "push") {
    const pc = j.push ? 100 * j.push.sent / j.push.total : 0;
    p.append(h("p", { id: "wi-push-text" }, j.push ? t("job.pushing", j.push.name, size(j.push.sent), size(j.push.total)) : t("job.push.start")));
    const bar = h("span", { id: "wi-push-bar" }); bar.style.width = pc.toFixed(1) + "%";
    p.append(h("div", { class: "wi-progress" }, bar));
    p.append(h("p", { class: "small muted" }, t("job.push.note")));
  }
  if (j.phase === "hash") p.append(wait(t("job.hash")));
  if (j.phase === "take") p.append(wait(t("job.take")));
  if (j.phase === "password") p.append(wait(t("job.password")));
  if (j.phase === "start") p.append(wait(t("job.start")));
  const names = kind === "uninstall" ? UNINSTALL_STEPS : INSTALL_STEPS;
  if (j.phase === "follow" || j.phase === "end") {
    p.append(h("ol", { class: "wi-steps" }, names.map((n) => h("li", { class: j.done.includes(n) ? "done" : j.now === n ? "now" : "" }, h("span", {}, t("step." + n)), h("span", { class: "small" }, t("step." + n + ".text"))))));
  }
  if (j.detached) p.append(callout("", t("job.detached.title"), t("job.detached.text")));
  if (j.phase === "end") {
    if (j.exit === 3 && j.rebootNeeded) {
      p.append(callout("", t("job.reboot.title"), t("job.reboot.text"), h("div", { class: "actions" }, btn(t("pending.button"), restartAndroid, "primary", { id: "wi-reboot" }))));
    } else if (j.exit === 0) p.append(callout("linux", t("job.ok." + kind), t("job.ok." + kind + ".text")));
    else p.append(callout("warn", t("job.failed"), j.error || t("job.failed.code", j.exit === null ? "?" : j.exit), t("job.failed.text")));
    p.append(h("div", { class: "actions" }, btn(t("job.close"), () => { S.job = null; render(); }, "small")));
  }
  return p;
}

function panelManage() {
  const st = S.status;
  if (!st || deviceBlockers(st).length || (!st.installed && !st.backup)) return null;
  const p = h("section", { class: "panel wi-panel", "aria-labelledby": "wi-man-h" }, h("h2", { id: "wi-man-h" }, t("man.title")));
  // start Linux once
  if (st.installed) {
    p.append(h("h3", {}, t("boot.title")));
    p.append(h("p", {}, t("boot.text")));
    const bb = bootBlockers(st);
    if (bb.length) p.append(callout("warn", t("boot.blocked"), blockerList(bb)));
    else if (!S.boot.dry) {
      if (S.boot.error) p.append(callout("warn", t("boot.check.failed"), S.boot.error));
      p.append(h("div", { class: "actions" }, btn(t("boot.check"), bootCheck, "", { id: "wi-boot-check" })));
    } else {
      p.append(h("p", { class: "wi-good" }, t("boot.check.ok")));
      p.append(h("label", { class: "wi-opt" }, h("input", { type: "checkbox", checked: S.boot.confirm, onchange: (ev) => { S.boot.confirm = ev.target.checked; render(); } }), h("span", {}, t("boot.confirm"))));
      p.append(h("div", { class: "actions" }, btn(t("boot.button"), bootLinux, "primary", { id: "wi-boot", disabled: !!S.busy || !S.boot.confirm })));
    }
    // uninstall
    p.append(h("h3", {}, t("un.title")));
    p.append(h("p", {}, t("un.text")));
    if (jobRunning(st)) p.append(h("p", {}, t("block.job-running")));
    else if (!S.verified) p.append(h("p", { class: "small" }, t("un.needfiles")));
    else {
      if (st.region && st.region.state !== "absent") {
        p.append(h("label", { class: "wi-opt" }, h("input", { type: "checkbox", checked: S.un.keep, onchange: (ev) => { S.un.keep = ev.target.checked; render(); } }),
          h("span", {}, h("strong", {}, t("un.keep")), h("br"), h("span", { class: "small" }, t("un.keep.text", st.region.size ? size(st.region.size) : "")))));
      }
      p.append(h("label", { class: "wi-opt" }, h("input", { type: "checkbox", checked: S.un.confirm, onchange: (ev) => { S.un.confirm = ev.target.checked; render(); } }), h("span", {}, t("un.confirm"))));
      p.append(h("div", { class: "actions" }, btn(t("un.button"), uninstall, "danger", { id: "wi-uninstall", disabled: !!S.busy || !S.un.confirm })));
    }
  }
  // backup
  if (st.backup) {
    p.append(h("h3", {}, t("bk.title")));
    p.append(h("p", {}, t("bk.text")));
    if (S.backup.error) p.append(callout("warn", "", S.backup.error));
    if (!S.backup.files) p.append(h("div", { class: "actions" }, btn(t("bk.list"), listBackup, "", { id: "wi-backup" })));
    else {
      p.append(h("ul", { class: "wi-filelist" }, S.backup.files.map((f) => h("li", {},
        btn(t("bk.download"), () => downloadBackup(f.name), "small"), " ", h("span", { class: "wi-mono" }, f.name), " ", h("span", { class: "small muted" }, size(f.size)),
        S.backup.got[f.name] === "ok" ? [" ", chip(t("bk.ok"), "ok")] : S.backup.got[f.name] === "unchecked" ? [" ", chip(t("bk.saved"), "")] : null))));
    }
  }
  return p;
}

let logBox = null;
function buildLog() {
  const pre = h("pre", { class: "wi-log", id: "wi-log-pre", tabindex: "0" }, LOG.map((l) => l + "\n").join(""));
  const keyLine = h("p", { class: "small muted", id: "wi-key" });
  logBox = h("details", { class: "wi-logbox" },
    h("summary", {}, t("log.title"), " (", h("span", { id: "wi-log-count" }, String(LOG.length)), ")"),
    h("p", { class: "small" }, t("log.text")),
    pre,
    h("div", { class: "actions" }, h("button", { type: "button", class: "wi-btn small", onclick: saveLog, id: "wi-save-log" }, t("log.save")),
      h("a", { class: "wi-btn small quiet", href: ISSUES }, t("log.report"))),
    keyLine);
  return logBox;
}

function render() {
  if (!root) return;
  root.hidden = false;
  let main = document.getElementById("wi-main");
  if (!main) {
    root.textContent = "";
    main = h("div", { id: "wi-main", class: "wi" });
    root.append(main, buildLog());
  }
  const scroll = window.scrollY;
  main.textContent = "";
  if (S.notice) main.append(callout(S.notice.kind, "", S.notice.text, h("div", { class: "actions" }, btn(t("ok"), () => { S.notice = null; render(); }, "small", { always: true }))));
  main.append(panelDevice());
  const job = panelJob();
  if (job) main.append(job);
  if (!S.probe || !propBlockers(S.probe.props).length || DEMO) main.append(panelRelease());
  const inst = panelInstall();
  if (inst && !(S.job && S.job.phase !== "end")) main.append(inst);
  const man = panelManage();
  if (man && !(S.job && S.job.phase !== "end")) main.append(man);
  const k = document.getElementById("wi-key");
  if (k) k.textContent = S.keyFp ? t("log.key", S.keyFp) + (S.keyInfo && !S.keyInfo.persistent ? " " + t("log.key.temp") : "") : "";
  window.scrollTo(0, scroll);
}

// ---------------------------------------------------------------------------------------------------- the demo

async function demoConnect(sameDev) {
  const demo = await import("./demo.js");
  const d = sameDev || demo.makeDemo(DEMO, (l) => logLine(l));
  S.usb = { productName: "U30 Pro", manufacturerName: "ZTE", serialNumber: d.serial };
  masker.add(d.serial);
  await ensureKey();
  S.conn = { close: async () => {} };
  S.phase = "connecting"; render();
  await new Promise((r) => setTimeout(r, 400));
  await afterConnect(d);
}

// ---------------------------------------------------------------------------------------------------- start

if (navigator.usb && !DEMO) {
  navigator.usb.addEventListener("disconnect", async (ev) => {
    if (S.usb === ev.device && S.conn) S.conn.close();
    await loadGranted();
    render();
  });
  navigator.usb.addEventListener("connect", async (ev) => {
    await loadGranted();
    render();
    if (S.lost) reconnectSoon(ev.device);
  });
  loadGranted().then(render);
}
window.addEventListener("pagehide", () => { if (S.conn) S.conn.close(); });
window.__mu5358 = { state: S, strings: STR, render };       // for a look from the browser's console; nothing reads it
render();
loadReleaseList();
