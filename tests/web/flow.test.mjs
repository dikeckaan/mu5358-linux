// flow.js: what the page decides by itself - the password rules, a release's SHA256SUMS, the device script's status
// line and its other lines.
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import { classifyLine, parseStatus, parseSums, passwordProblem, validPassword, bootBlockers, installBlockers,
  REGION_MIN, ROOT_MIB, DATA_NEED, jobKind } from "../../site/assets/web-installer/flow.js";

const TOP = new URL("../../", import.meta.url);

test("validPassword: 6 to 128 characters, one line, no blank at the ends, not wrapped in quotes", () => {
  for (const ok of ["abcdef", "a".repeat(128), "pass word", "şifre123", "密码密码密码", "it's \"fine\"", "'start", "the end'", "a\"b'c!@#$%^&*()"]) {
    assert.equal(validPassword(ok), true, JSON.stringify(ok));
  }
  for (const bad of ["", "abcde", "a".repeat(129), " abcdef", "abcdef ", "abc\ndef", "abc\tdef", "abc\x7fdef", "abc\0def",
    "\"abcdef\"", "'abcdef'", null, undefined, 123456, ["abcdef"]]) {
    assert.equal(validPassword(bad), false, JSON.stringify(bad));
  }
});

test("passwordProblem: the code of what is wrong", () => {
  assert.equal(passwordProblem("abc", "abc"), "password");
  assert.equal(passwordProblem("abcdef", "abcdeg"), "password-differs");
  assert.equal(passwordProblem("abcdef", "abcdef"), "");
});

const FIXTURE = `# version v2026.10.30.6
# image mu5358-openwrt.img.gz d9bb25ee338239a6b8f2a65a38a4688ebf4a49ec0a31e6ab376ff53b022d4031 2146435072
1aeab11d3d7b0dbe72c15ad38807a377a0109d84957f4eae04e5cfb01656a4b7  mu5358-openwrt.img.gz
a241ff1048d00862173402902b733220cbcb6a95d7ff901e14a94d739040b9fe  mu5358-kernel.Image.gz
041617a176e1d95970dc4726dd8cf234460ba90293e1257db4d3effa0b673ca9  mu5358-ramdisk.tar.gz
c56249047ece8645d3185d7e2fed5cd8be7453851f2f4630d75e54ea30b24453  mu5358-tools.tar.gz
931a1f5e7732ab02a6316f02a468a0352953a318d810e24976aeaaea6768c5d2  mu5358-install.sh
991bf48144ae02ccee5e8ed725b44c00523a03f56df541810a73de7fce9191cf  android-files.txt
`;

function releaseSums() {
  // the newest release built here (work/release, not in git), else the fixture above
  const dir = new URL("work/release/", TOP);
  try {
    const tags = fs.readdirSync(dir).filter((t) => fs.existsSync(path.join(dir.pathname, t, "SHA256SUMS"))).sort();
    if (tags.length) return { from: tags.at(-1), text: fs.readFileSync(path.join(dir.pathname, tags.at(-1), "SHA256SUMS"), "utf8") };
  } catch (e) { /* no work/release */ }
  return { from: "fixture", text: FIXTURE };
}

test("parseSums: the version, the image line and every file", () => {
  const { from, text } = releaseSums();
  const s = parseSums(text);
  assert.match(s.version, /^v\d{4}\.\d{2}\.\d{2}(\.\d+)?$/, from);
  assert.ok(s.image, `${from}: no image line`);
  assert.equal(s.image.name, "mu5358-openwrt.img.gz");
  assert.match(s.image.sha256, /^[0-9a-f]{64}$/);
  assert.ok(s.image.size > 1e9 && s.image.size % 4096 === 0, `${from}: image size ${s.image.size}`);
  for (const f of ["mu5358-openwrt.img.gz", "mu5358-kernel.Image.gz", "mu5358-ramdisk.tar.gz", "mu5358-install.sh", "android-files.txt"]) {
    assert.match(s.files.get(f) || "", /^[0-9a-f]{64}$/, `${from}: ${f}`);
  }
  // the image's own hash differs from its gzip's
  assert.notEqual(s.files.get("mu5358-openwrt.img.gz"), s.image.sha256);
});

test("parseSums: CRLF, a binary-mode star, a second version line ignored, junk left out", () => {
  const s = parseSums("# version v1\r\n# version v2\r\n" + "a".repeat(64) + " *file.gz\r\nnot a line\r\n" + "b".repeat(63) + "  short.gz\r\n");
  assert.equal(s.version, "v1");
  assert.equal(s.image, null);
  assert.deepEqual([...s.files], [["file.gz", "a".repeat(64)]]);
  assert.deepEqual([...parseSums("").files], []);
  assert.deepEqual([...parseSums(null).files], []);
});

const STATUS = 'MU5358-STATUS {"model":"MU5358","device":"MU5358","platform":"ums9632","serial":"324761041479","android":"15","build":"MU5358V1.0.0B19","slot":"_a","magisk":"29.0:MAGISK","magiskboot":true,"misc":"android","boot_b":"linux","fulldumpdb":"empty","fulldumpdb_size":2147483648,"region":{"state":"ours","start":113745920,"size":4296015872},"userdata":{"start":1024000,"size":58226176000,"kernel_size":58226176000},"root":"region","installed":"v2026.10.30.6","backup":true,"job":"exit-0","free_data":40000000000}';

test("parseStatus: the installer's line, every field", () => {
  const st = parseStatus(STATUS);
  assert.ok(st);
  assert.equal(st.model, "MU5358");
  assert.equal(st.platform, "ums9632");
  assert.equal(st.slot, "_a");
  assert.equal(st.magiskboot, true);
  assert.equal(st.misc, "android");
  assert.equal(st.boot_b, "linux");
  assert.equal(st.fulldumpdb, "empty");
  assert.equal(st.fulldumpdb_size, 2147483648);
  assert.deepEqual(st.region, { state: "ours", start: 113745920, size: 4296015872 });
  assert.deepEqual(st.userdata, { start: 1024000, size: 58226176000, kernel_size: 58226176000 });
  assert.equal(st.root, "region");
  assert.equal(st.installed, "v2026.10.30.6");
  assert.equal(st.backup, true);
  assert.equal(st.job, "exit-0");
  assert.equal(st.free_data, 40000000000);
  assert.deepEqual(installBlockers(st), []);
  assert.deepEqual(bootBlockers(st), []);
  // the same line with CR and trailing blanks
  assert.deepEqual(parseStatus(STATUS + "  \r"), st);
});

test("parseStatus: what is not a status line, and odd values made safe", () => {
  assert.equal(parseStatus("MU5358-STEP check"), null);
  assert.equal(parseStatus("MU5358-STATUS not json"), null);
  assert.equal(parseStatus("MU5358-STATUS []"), null);
  assert.equal(parseStatus(""), null);
  const st = parseStatus('MU5358-STATUS {"misc":"magic","boot_b":5,"fulldumpdb":null,"magiskboot":"true","backup":1,"job":"exit-x","free_data":"abc","region":"ours","root":"/"}');
  assert.equal(st.misc, "unknown");
  assert.equal(st.boot_b, "unknown");
  assert.equal(st.fulldumpdb, "unknown");
  assert.equal(st.magiskboot, false);
  assert.equal(st.backup, false);
  assert.equal(st.job, "none");
  assert.equal(st.free_data, null);
  assert.equal(st.region, null);
  assert.equal(st.root, "");
  assert.ok(bootBlockers(st).includes("misc-unknown"));
  assert.ok(bootBlockers(st).includes("nothing-installed"));
});

test("classifyLine: the script's marks", () => {
  assert.deepEqual(classifyLine("MU5358-STEP backup"), { type: "step", name: "backup" });
  assert.deepEqual(classifyLine("MU5358-EXIT 3\r"), { type: "exit", code: 3 });
  assert.deepEqual(classifyLine("MU5358-ERROR no password set"), { type: "error", text: "no password set" });
  assert.deepEqual(classifyLine("MU5358-INFO mu5358-kernel.Image.gz ok"), { type: "info", text: "mu5358-kernel.Image.gz ok" });
  assert.deepEqual(classifyLine("MU5358-TAKEN v2026.10.30.6"), { type: "taken", version: "v2026.10.30.6" });
  assert.deepEqual(classifyLine("MU5358-JOB started"), { type: "job" });
  assert.deepEqual(classifyLine("MU5358-PASSWORD set"), { type: "password" });
  assert.deepEqual(classifyLine("MU5358-REBOOT-NEEDED"), { type: "reboot-needed" });
  assert.deepEqual(classifyLine("MU5358-REBOOT"), { type: "reboot" });
  assert.deepEqual(classifyLine("MU5358-FILE misc-4k.bin 4096"), { type: "file", name: "misc-4k.bin", size: 4096 });
  assert.equal(classifyLine(STATUS).type, "status");
  assert.deepEqual(classifyLine("the region: sectors 1..2"), { type: "text", text: "the region: sectors 1..2" });
});

test("the constants match the device script", () => {
  const s = fs.readFileSync(new URL("installer/mu5358-install.sh", TOP), "utf8");
  assert.ok(s.includes(`[ "$_mib" -ge ${REGION_MIN} ]`), "REGION_MIN");
  assert.ok(s.includes(`ROOT_MIB=${ROOT_MIB} `), "ROOT_MIB");
  assert.ok(s.includes(`[ "$_free" -ge $((${DATA_NEED / 1024 / 1024} * 1024)) ]`), "DATA_NEED");
});

// jobKind: the device script's "update" (keeps the settings, no password) when a system is installed where the user
// points; "install" (fresh, a password) everywhere else. Found 2026-11-01: the page said "Install again (update)" and
// ran install, which would have wiped the installed system's settings.
test("jobKind: update where a system is installed, install elsewhere", () => {
  const st = { installed: "v2026.11.01.4", root: "region" };
  assert.equal(jobKind(st, "region"), "update");
  assert.equal(jobKind(st, "fulldumpdb"), "install");
  assert.equal(jobKind({ installed: "", root: "" }, "region"), "install");
  assert.equal(jobKind(null, "region"), "install");
});
