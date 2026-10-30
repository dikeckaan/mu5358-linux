// What the page asks a U30 Pro (MU5358) in Android, over adb: a few properties without root, and otherwise the
// device script, installer/mu5358-install.sh, as root:
//
//   su -c 'sh /data/local/tmp/mu5358-web/mu5358-install.sh COMMAND ARG...'
//
// The page pushes that script (and android-files.txt) only after it has checked them against the release's
// SHA256SUMS, whose own checksum GitHub publishes. Before the user has picked a release, the page may run the copy
// the device's last installation keeps in root's own directory (/data/adb/mu5358, which the adb user cannot write)
// for the commands that change nothing but what the user asked for in that visit: status, follow, boot-linux,
// backup, cat. An installation and an uninstallation always run the script the user picked in this visit.
//
// Every line the device prints goes to the log masked (privacy.js); the backup files' base64 never goes to the log.
import { AdbError } from "./adb/errors.js";
import { shell } from "./adb/shell.js";
import { push } from "./adb/sync.js";
import { classifyLine, parseStatus } from "./flow.js";
import { mask as pageMask, pageMasker } from "./privacy.js";
import { F } from "./github.js";
import { isHash } from "./sha512crypt.js";

export const STAGE = "/data/local/tmp/mu5358-web";
export const STAGED_SCRIPT = `${STAGE}/mu5358-install.sh`;
export const ROOT_COPY = "/data/adb/mu5358/mu5358-install.sh";
// the release's files the script takes from STAGE/in (its take command); the script and its list go to STAGE
export const PUSHED_IN = [F.sums, F.kernel, F.ramdisk, F.image, F.tools];      // F.tools when the release has it
export const BACKUP_FILES = ["boot_b.img.gz", "init_boot_b.img.gz", "misc-4k.bin", "SHA256SUMS"];

export class DeviceError extends Error {
  // code: a word the page has a sentence for; detail: the device's own words (masked)
  constructor(code, detail) { super(detail ? `${code}: ${detail}` : code); this.name = "DeviceError"; this.code = code; this.detail = detail || ""; }
}

// an argument of the device script: a plain word, nothing a shell would read as more than one
const ARG = /^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/;
export const safeArg = (a) => typeof a === "string" && ARG.test(a) && !a.includes("..");

const PROPS = ["ro.product.model", "ro.product.device", "ro.board.platform", "ro.boot.slot_suffix", "ro.build.version.release",
  "ro.build.display.id", "sys.boot_completed"];
const lines = (s) => s.split("\n").map((l) => l.replace(/\r+$/, ""));

function base64ToBytes(s) {
  const clean = s.replace(/[^A-Za-z0-9+/=]/g, "");
  const bin = atob(clean);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

export class Mu5358Device {
  constructor(conn, options = {}) {
    this.conn = conn;
    this.mask = options.mask || pageMask;
    const log = options.log || (() => {});
    this.log = (l, isErr) => log(this.mask(l), isErr);
    this.script = "";          // STAGED_SCRIPT once pushed in this visit, else ROOT_COPY when the device has one
  }

  // { props, su } - no root needed
  async probe() {
    const cmd = PROPS.map((p) => `echo "${p}=$(getprop ${p})"`).join("; ") + "; command -v su >/dev/null 2>&1 && echo SU=1 || echo SU=0";
    const r = await shell(this.conn, cmd, { timeoutMs: 20000 });
    const props = {};
    let su = false;
    for (const l of lines(r.stdout)) {
      const eq = l.indexOf("=");
      if (eq <= 0) continue;
      if (l.startsWith("SU=")) su = l === "SU=1"; else props[l.slice(0, eq)] = l.slice(eq + 1);
    }
    return { props, su };
  }

  // "granted" | "denied" | "no-su"; the first call may wait for the user to allow root in Magisk on the device
  async root(timeoutMs = 60000) {
    let r;
    try { r = await shell(this.conn, "su -c 'id -u' 2>&1", { timeoutMs }); } catch (e) {
      if (e instanceof AdbError && e.code === "timeout") return "denied";
      throw e;
    }
    const out = r.stdout.trim().split(/\s+/).pop() || "";
    if (out === "0") return "granted";
    if (/not found|inaccessible|No such file/i.test(r.stdout + r.stderr)) return "no-su";
    return "denied";
  }

  // whether root's copy of the script is there (a device that had an installation)
  async findRootCopy() {
    const r = await shell(this.conn, `su -c 'test -f ${ROOT_COPY} && echo HAVE || echo NONE'`, { timeoutMs: 20000 });
    if (/HAVE/.test(r.stdout)) { if (!this.script) this.script = ROOT_COPY; return true; }
    return false;
  }

  // the checked script and its list, to STAGE (as the adb user)
  async pushScripts(files) {
    const r = await shell(this.conn, `mkdir -p ${STAGE}/in && chmod 755 ${STAGE} ${STAGE}/in && echo STAGE-OK`, { timeoutMs: 20000 });
    if (!r.stdout.includes("STAGE-OK")) throw new DeviceError("no-stage", this.mask((r.stdout + r.stderr).trim()).slice(0, 200));
    await push(this.conn, files.get(F.script), STAGED_SCRIPT, { mode: 0o644 });
    await push(this.conn, files.get(F.list), `${STAGE}/${F.list}`, { mode: 0o644 });
    this.script = STAGED_SCRIPT;
  }

  // the release's files, to STAGE/in; onProgress(name, sent, total) over all of them
  async pushRelease(files, onProgress = () => {}, signal) {
    const names = PUSHED_IN.filter((n) => files.has(n));
    const total = names.reduce((s, n) => s + files.get(n).size, 0);
    let before = 0;
    for (const name of names) {
      const f = files.get(name);
      await push(this.conn, f, `${STAGE}/in/${name}`, { mode: 0o644, signal, onProgress: (sent) => onProgress(name, before + sent, total) });
      before += f.size;
    }
  }

  // One command of the script, as root. -> { rc, out: [lines as they came: mask before showing] }. options: onEvent(classified, maskedLine),
  // quiet (lines not logged: the caller does it), raw (onStdout: the output as it comes, not split, not logged),
  // stdin (text the command reads: never in the command line), timeoutMs, signal.
  async cmd(args, options = {}) {
    if (!this.script) throw new DeviceError("no-script");
    for (const a of args) if (!safeArg(a)) throw new DeviceError("bad-argument", this.mask(String(a)).slice(0, 40));
    const command = `su -c 'sh ${this.script} ${args.join(" ")}'`;
    const out = [];
    const r = await shell(this.conn, command, {
      timeoutMs: options.timeoutMs, signal: options.signal, stdin: options.stdin,
      onStdout: options.raw || undefined,
      onLine: options.raw ? undefined : (l, isErr) => {
        const ev = classifyLine(l);
        const shown = this.mask(l);
        out.push(l);
        if (!options.quiet) this.log(shown, isErr);
        if (options.onEvent) options.onEvent(ev, shown, l);
      },
    });
    return { rc: r.exitCode, out };
  }

  // the first MU5358-ERROR of an answer, as a DeviceError
  static failure(out, fallback) {
    const e = out.find((l) => l.startsWith("MU5358-ERROR"));
    return new DeviceError(e ? "script" : fallback, pageMask(e ? e.replace(/^MU5358-ERROR ?/, "") : out.slice(-3).join(" ")).slice(0, 300));
  }

  async status() {
    let st = null;
    const r = await this.cmd(["status"], { quiet: true, timeoutMs: 60000, onEvent: (ev, shown, raw) => { if (ev.type === "status") st = parseStatus(raw); } });
    // the serial number is masked from here on, everywhere (the status line itself included)
    if (st && st.serial) pageMasker.add(st.serial);
    for (const l of r.out) this.log(l);          // this.log masks, now with the serial number
    if (!st) throw Mu5358Device.failure(r.out, "no-status");
    return st;
  }

  async take(sumsSha) {
    let version = null;
    const r = await this.cmd(["take", sumsSha], { timeoutMs: 600000, onEvent: (ev) => { if (ev.type === "taken") version = ev.version; } });
    if (version === null) throw Mu5358Device.failure(r.out, "take-failed");
    return version;
  }

  // The root password of the system the next `run install` writes, as its sha512-crypt hash ("$6$salt$hash", made
  // by the page: the device never sees the password itself). It goes on the command's stdin, one line; the script
  // keeps it for the next run install and refuses to install without one. The log shows "[password hash]" for it.
  async password(hash) {
    if (!isHash(hash)) throw new DeviceError("bad-argument", "password");
    let set = false;
    const r = await this.cmd(["password"], { stdin: hash + "\n", timeoutMs: 30000, onEvent: (ev) => { if (ev.type === "password") set = true; } });
    if (!set) throw Mu5358Device.failure(r.out, "password-failed");
  }

  // run install region [MIB] | run install fulldumpdb | run uninstall [keep-space]
  async run(kind, ...args) {
    if (this.script !== STAGED_SCRIPT) throw new DeviceError("no-script");
    let started = false;
    const r = await this.cmd(["run", kind, ...args.map(String)], { timeoutMs: 60000, onEvent: (ev) => { if (ev.type === "job") started = true; } });
    if (!started) throw Mu5358Device.failure(r.out, "job-not-started");
  }

  // the job's log from its start; onEvent(ev, maskedLine) for every line. -> its exit status
  async follow(onEvent, signal) {
    let exit = null;
    const r = await this.cmd(["follow"], { signal, onEvent: (ev, shown) => { if (ev.type === "exit") exit = ev.code; onEvent(ev, shown); } });
    if (exit === null) throw Mu5358Device.failure(r.out, "follow-ended");
    return exit;
  }

  // dry: -> the trial block it would write. Otherwise resolves once the device said it reboots (the connection then
  // goes away, which is not a failure).
  async bootLinux(dry) {
    let rebooting = false, info = "";
    try {
      const r = await this.cmd(dry ? ["boot-linux", "dry"] : ["boot-linux"], { timeoutMs: 60000, onEvent: (ev) => {
        if (ev.type === "reboot") rebooting = true;
        if (ev.type === "info") info = ev.text;
      } });
      if (dry ? !info : !rebooting) throw Mu5358Device.failure(r.out, "boot-failed");
    } catch (e) {
      if (!dry && rebooting && e instanceof AdbError) return { rebooting: true };
      throw e;
    }
    return dry ? { info } : { rebooting: true };
  }

  // Android's own restart (adb's reboot service, as `adb reboot` does): after a shrink, the new partition table
  // takes effect at the next start. The connection goes away; that is the answer.
  async reboot() {
    try {
      const s = await this.conn.open("reboot:");
      try { await s.read(); } catch (e) { /* gone */ }
    } catch (e) {
      if (!(e instanceof AdbError)) throw e;
      if (e.code === "disconnected" || e.code === "closed") return;
      await shell(this.conn, "su -c reboot", { timeoutMs: 15000 }).catch(() => {});
    }
  }

  async backupList() {
    const files = [];
    const r = await this.cmd(["backup"], { timeoutMs: 30000, onEvent: (ev) => { if (ev.type === "file" && BACKUP_FILES.includes(ev.name)) files.push({ name: ev.name, size: ev.size }); } });
    if (!files.length) throw Mu5358Device.failure(r.out, "no-backup");
    return files;
  }

  // one backup file's bytes (the script prints it as base64; that text is not logged)
  async cat(name) {
    if (!BACKUP_FILES.includes(name)) throw new DeviceError("bad-argument", name);
    const parts = [];
    const r = await this.cmd(["cat", name], { timeoutMs: 600000, raw: (t) => parts.push(t) });
    const textValue = parts.join("");
    if (/MU5358-ERROR/.test(textValue)) throw new DeviceError("script", this.mask(textValue.trim()).slice(0, 200));
    if (r.rc !== 0 && r.rc !== null) throw new DeviceError("cat-failed", name);
    return base64ToBytes(textValue);
  }
}
