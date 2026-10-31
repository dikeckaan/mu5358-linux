# First work package: CI release + Telegram, kernel fragment round 1, charging — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A signed release built and announced by CI, a kernel that exposes I2C to userspace, and a charging
service that takes 1.5 A from the Mac (what Android takes) instead of 500 mA.

**Architecture:** The Release workflow already builds everything; this adds the Telegram step and a gate on the
kernel fragment. The fragment grows four module options (nothing the vendor modules see). `mu5358-charge` is one
static C program: a pure decision (`decide`) from power-supply sysfs, an I2C apply to the master bq2560x, a state
file for the UIs; a procd service respawns it. Tests: Python unittest (static rules), a host build of the C
decision logic against fake sysfs trees, the bench for the current.

**Tech Stack:** POSIX sh, bash (CI), C (static aarch64, built in `mu5358-mainline-build`), GitHub Actions,
Python unittest, busybox/procd (OpenWrt).

**Spec:** `docs/superpowers/specs/2026-11-01-mu300-parity-usb-charging-design.md` (W8, W6 round 1, W4).

## Global Constraints

- Kernel release string stays `6.6.98-android15-8-mu5358-4k`; every symbol CRC equal to `kernel/Module.symvers`
  (ci-release.sh's gate); fragment options are modules or code-only additions.
- Nothing writes slot a, `misc`, `nr_*nv*`, `prodnv` (tests/test_static hard rules apply to every new script).
- Secrets (`MU5358_RELEASE_KEY`, `TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID`) never on a command line or in a log:
  curl reads its URL from a file (`-K`).
- `mu5358-charge` writes only the master charger (`/dev/i2c-6`, address `0x6b`), only `REG00` (IINLIM bits 4:0,
  EN_HIZ kept 0) and `REG01` bit 4 (CHG_CONFIG); never the slave (`4-006b`); never VREG/ICHG.
- Current limits this round: SDP 500 mA, CDP 1500, DCP 2000, Type-C with `current_max ≥ 1500000` or a PD type
  1500, anything unknown 500; the daemon's hard cap `1500`.
- Temperature guard: charging off at battery `temp ≥ 450` (deci-°C) or `≤ 0`, back on at `≤ 400` and `> 0`.
  Charge limit: `/etc/mu5358/charge-limit` N in 50..100 (default 100 = off): off at `capacity ≥ N`, on at
  `≤ N-3`.
- No bench test cuts the USB link to the Mac. The SIM may be used only for the update check (not in this package).
- Trial boots follow the standing rule: slot a's hash equals the backup's before the first trial of a new build.

## Review Focus

1. A source that disappears while charging (unplug): the daemon must not keep a 1500 mA limit for the next plug
   of an SDP host; test: `decide` with `usb/online=0` → `input_ma=500, charge=on, source=none` (Task 4).
2. A TCPM psy missing (module not loaded) or a sysfs file unreadable: the decision falls back to BC1.2, then to
   500 mA; never a crash; test: a tree without the tcpm directory (Task 4).
3. The vendor driver rewriting REG00: the daemon re-applies every cycle and counts the rewrites in the state file
   (`rewrites=N`); test: apply twice with the register changed between (Task 5, with a fake I2C file).
4. A battery temperature file reading garbage or `-1`: treated as unknown → charging on, 500 mA; test (Task 4).
5. `mu5358-charge` killed (respawn, reboot): the chip is left at 500 mA/charging on by the SIGTERM path, and a
   fresh start reads the register before writing; test: `once` after a state file from a previous run (Task 5).

---

### Task 1: The first CI release through to the page, and the fragment gate

**Files:**
- Modify: `tools/ci-release.sh` (after the kernel step)
- Modify: `docs/MU300-PARITY.md` (12b status line), `README.md` (release section mentions the CI release)

**Interfaces:**
- Produces: a prerelease `vYYYY.MM.DD.N` on GitHub with 8 assets and the `mirror/<tag>` branch; `work/ci/` record.

- [ ] **Step 1: Watch run 41248161525** (`gh run watch 41248161525 --repo dikeckaan/mu5358-linux --exit-status`
  under a Monitor, not a foreground sleep). On failure read `gh run view --log-failed`, fix, push, dispatch again
  (`gh workflow run release.yml --repo dikeckaan/mu5358-linux --ref main`), repeat until green.
- [ ] **Step 2: Verify the release** — Run:
  `gh api repos/dikeckaan/mu5358-linux/releases/tags/TAG --jq '.prerelease, (.assets[]|"\(.digest) \(.name)")'`
  Expected: `true`, 8 lines, every digest `sha256:…`, and
  `curl -sI https://raw.githubusercontent.com/dikeckaan/mu5358-linux/refs/heads/mirror/TAG/manifest.json | grep -i access-control-allow-origin` → `*`.
- [ ] **Step 3: Verify the page** — Playwright on https://kaandikec.com/mu5358-linux/web-installer/ : the release
  list shows TAG marked as a test release and still proposes the newest non-prerelease; 0 console errors.
- [ ] **Step 4: Fragment gate in `tools/ci-release.sh`** — after the KMI check add:
  `grep -q . kernel/fragment-rejected.txt && { echo "the fragment was not fully applied:"; cat kernel/fragment-rejected.txt; exit 1; }`
  (the file always holds at least a newline: `grep -q .` is the test). Add to `tests/test_static.py` class
  `ReleaseWorkflow` a test `test_fragment_gate` asserting `'fragment-rejected.txt' in ci` and that it comes after
  `'KMI:'` in the script text.
- [ ] **Step 5: Run** `python3 -m unittest tests.test_static -q` → OK. Commit:
  `git commit -m "ci-release: a fragment option olddefconfig dropped fails the build"`.

### Task 2: Telegram announcements

**Files:**
- Create: `tools/telegram-announce.sh` (from `M/tools/telegram-announce.sh`)
- Create: `.github/workflows/telegram.yml` (from `M/.github/workflows/telegram.yml`)
- Modify: `.github/workflows/release.yml` (a step after "build and publish")
- Test: `tests/test_static.py` (class `Telegram`)

**Interfaces:**
- Produces: `sh tools/telegram-announce.sh release TAG [published|released]`; env `TELEGRAM_BOT_TOKEN`,
  `TELEGRAM_CHAT_ID`, `GH_TOKEN`; `MU5358_TG_DRY=1` prints the message instead of sending. Exit 0 sent/dry,
  1 Telegram refused, 2 usage.

- [ ] **Step 1: Write the failing tests** in `tests/test_static.py`:

```python
class Telegram(unittest.TestCase):
    S = TOP / 'tools/telegram-announce.sh'
    def test_token_never_on_argv(self):
        t = self.S.read_text()
        self.assertIn('curl -sS -K', t)
        self.assertNotRegex(t, r'curl[^\n]*TELEGRAM_BOT_TOKEN')
        self.assertNotRegex(t, r'api\.telegram\.org/bot\$')  # the URL is built into the config file only
    def test_dry_run_message(self):
        env = dict(os.environ, MU5358_TG_DRY='1', TELEGRAM_BOT_TOKEN='x', TELEGRAM_CHAT_ID='1', PATH=f'{FAKEBIN}:{os.environ["PATH"]}')
        out = subprocess.run(['sh', str(self.S), 'release', 'v2026.11.01.1', 'published'], env=env, capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn('MU5358 Linux v2026.11.01.1', out.stdout)
        self.assertIn('web-installer', out.stdout)
    def test_bad_tag_refused(self):
        for tag in ('main', 'v2026.11.01.1;x', '../x'):
            self.assertEqual(subprocess.run(['sh', str(self.S), 'release', tag], capture_output=True).returncode, 2)
    def test_release_workflow_announces(self):
        y = (TOP / '.github/workflows/release.yml').read_text()
        self.assertIn('tools/telegram-announce.sh release "$TAG" published', y)
        self.assertLess(y.index('tools/ci-release.sh'), y.index('telegram-announce.sh'))
        self.assertIn("continue-on-error: true", y[y.index('telegram-announce.sh') - 400:y.index('telegram-announce.sh')])
```
`FAKEBIN` is a directory under `tests/fixtures/fakebin/` holding a `gh` script that prints
`{"name":"v2026.11.01.1","isPrerelease":true,"publishedAt":"2026-11-01T00:00:00Z","assets":[{"name":"a"}]}` for
`release view` (create it in this step; `jq` must exist on the test host, else the dry test skips with
`shutil.which('jq')`).
- [ ] **Step 2: Run** `python3 -m unittest tests.test_static.Telegram -v` → FAIL (file missing).
- [ ] **Step 3: Write `tools/telegram-announce.sh`**: mu300's with `mu300`→`mu5358`, `MU300_TG_DRY`→`MU5358_TG_DRY`,
  `REPO` default `dikeckaan/mu5358-linux`; the `zips` mode removed; the "What's new" block replaced by the first
  paragraph after the title of the release's notes (`gh release view TAG --json body --jq .body`, lines up to the
  first blank one, `esc`'d, at most 400 characters); the lead texts: prerelease "Under test on the board; it
  becomes the latest release once it passes. The web installer lists it as a test release.", release "Install or
  update from the web installer: https://kaandikec.com/mu5358-linux/web-installer/", released "Tested on the
  board and promoted; the web installer proposes it from now on."; the footer links Release page, Web installer,
  Site. Keep the `curl -K` config-file pattern verbatim.
- [ ] **Step 4: `.github/workflows/telegram.yml`**: mu300's with the repo name and `v` tag filter; `permissions:
  contents: read`.
- [ ] **Step 5: `release.yml`** — after "build and publish" add:
  ```yaml
      - name: Telegram
        if: success()
        continue-on-error: true
        env:
          TELEGRAM_BOT_TOKEN: ${{ secrets.TELEGRAM_BOT_TOKEN }}
          TELEGRAM_CHAT_ID: ${{ secrets.TELEGRAM_CHAT_ID }}
        run: |
          [ -n "$TELEGRAM_BOT_TOKEN" ] || { echo "no TELEGRAM_BOT_TOKEN: not announced"; exit 0; }
          sh tools/telegram-announce.sh release "$TAG" published
  ```
  (a release made with the workflow token fires no `release` event, as mu300's comment says: this step is the
  announcement of a CI release; `telegram.yml` covers the promotion by hand and a dispatch).
- [ ] **Step 6: Run** the Telegram tests and the whole suite (`python3 -m unittest discover -s tests -p 'test_*.py'`)
  → OK. `sh -n` both scripts. Commit: `git commit -m "Telegram: a CI release and a promotion announced (mu300's telegram.yml)"`.
- [ ] **Step 7: Live check** after the next CI release: the channel got one message; `gh run view` shows the step.

### Task 3: Kernel fragment round 1 and the i2c-dev module in the image

**Files:**
- Modify: `kernel/mu5358-linux.fragment` (append), `openwrt/customize.sh:126-133` (next to rfkill)
- Test: `tests/test_static.py` (class `KernelFragment`)

**Interfaces:**
- Produces: `/lib/mu5358/modules/i2c-dev.ko` loaded by `mu5358-hw` at boot (listed in `/etc/mu5358/modules` after
  `rfkill`); `/dev/i2c-6` and `/dev/i2c-4` on the device (devtmpfs). `spidev.ko`, `udl.ko`, RNDIS built but not
  loaded.

- [ ] **Step 1: Failing test**:
```python
class KernelFragment(unittest.TestCase):
    def test_round1_modules(self):
        f = (TOP / 'kernel/mu5358-linux.fragment').read_text()
        for line in ('CONFIG_I2C_CHARDEV=m', 'CONFIG_SPI_SPIDEV=m', 'CONFIG_DRM_UDL=m', 'CONFIG_USB_CONFIGFS_RNDIS=y', 'CONFIG_USB_F_RNDIS=y'):
            self.assertIn(line + '\n', f)
    def test_i2c_dev_in_image(self):
        c = (TOP / 'openwrt/customize.sh').read_text()
        self.assertIn('gki-modules-disabled/i2c-dev.ko', c)
        self.assertLess(c.index('echo rfkill >>'), c.index('echo i2c-dev >>'))
```
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Append to the fragment** (with a comment: "W6 round 1, design 2026-11-01: userspace I2C for
  mu5358-charge, SPI for the panel tools, DisplayLink DL-1x5, RNDIS for Windows; modules or code only"), the five
  lines above.
- [ ] **Step 4: `customize.sh`**: after the rfkill block, the same three lines for `i2c-dev` (copy from
  `gki-modules-disabled` when present, `echo i2c-dev >> /etc/mu5358/modules` when the .ko is in place).
- [ ] **Step 5: Run** the tests → OK; `sh -n openwrt/customize.sh`. Commit:
  `git commit -m "kernel: I2C_CHARDEV, SPI_SPIDEV, DRM_UDL, RNDIS as modules; i2c-dev loaded at boot"`.
- [ ] **Step 6: CI** — dispatch the Release workflow; expected in the log: `fragment rejected: 0`, `KMI: … CRC
  differs 0`, and `work/ci/linux.config` (artifact) has `CONFIG_I2C_CHARDEV=m`. If olddefconfig drops one (the
  gate of Task 1 fails the run): record it in `kernel/fragment-rejected.txt`'s commit message and drop that line,
  the rest stays.

### Task 4: `mu5358-charge` — the decision

**Files:**
- Create: `tools/charge/mu5358-charge.c`, `tools/charge/build.sh`
- Test: `tests/test_charge.py`, fixtures made by the test in a temp dir

**Interfaces:**
- Produces (C, in the one file):
  - `struct inputs { int usb_online, tcpm_online, tcpm_current_ua; char tcpm_type[16]; int ac_online; char ac_type[16]; int temp, capacity, limit; }`
  - `int read_inputs(const char *root, struct inputs *in)` — every missing/unparsable file leaves its field at
    -1 (ints) or "" (strings); returns 0 always.
  - `struct decision { int input_ma; int charge; char source[8]; char reason[32]; }`
  - `void decide(const struct inputs *in, const struct decision *prev, struct decision *out)` — pure; `prev`
    carries the hysteresis state (`charge`), may be NULL.
  - CLI `mu5358-charge decide` prints one line `source=… input_ma=… charge=on|off reason=…` from `$MU5358_ROOT`
    (default empty) + the sysfs paths, exit 0.
- Paths (relative to root): `/sys/class/power_supply/usb/online`,
  `/sys/class/power_supply/sprd-tcpm-source-psy-sc27xx-pd/{online,current_max,usb_type}` (the bracketed word of
  usb_type), `/sys/class/power_supply/ac/{online,type}`, `/sys/class/power_supply/battery/{temp,capacity}`,
  `/etc/mu5358/charge-limit`.
- Decision table: no VBUS (`usb_online != 1 && tcpm_online != 1`) → 500/on/`none`; tcpm type `PD` or `PD_PPS`,
  or `C` with `current_ua >= 1500000` → 1500/`typec`; `C` with less → by BC1.2: `ac_online==1` and `ac_type`
  `USB_DCP` → 2000? **No: the hard cap is 1500**, so DCP → 1500/`dcp`, `USB_CDP` → 1500/`cdp`; `USB`/SDP or
  unknown → 500/`sdp`. Then the guards in this order, each setting `charge=off` and `input_ma=500` with its
  reason: `temp >= 450` → `hot`; `temp <= 0 && temp != -1` → `cold`; hysteresis: if `prev->charge==0` for
  hot/cold and now `temp > 400 && temp > 0` → on, else stays off; `limit` in 50..99 and `capacity >= limit` →
  `limit`, back on at `capacity <= limit-3`; `temp == -1` → unknown: charge on, 500/`notemp`.

- [ ] **Step 1: Write `tests/test_charge.py`** — builds `mu5358-charge` for the host with `cc -O0 -o <tmp>/mc
  tools/charge/mu5358-charge.c` (skip when `shutil.which('cc')` is None), a helper `tree(**files)` writing a fake
  root, `run(root, *args)` returning stdout. Tests, with the expected lines verbatim:
  - `test_mac_typec_3a`: usb 1, tcpm online 1, current_max 3000000, usb_type `[C] PD PD_PPS`, ac 0, temp 274,
    capacity 23 → `source=typec input_ma=1500 charge=on reason=source`
  - `test_sdp`: tcpm current 500000 type `[C]`, ac online 0 → `source=sdp input_ma=500 charge=on reason=source`
  - `test_dcp`: tcpm current 500000, ac online 1 type `USB_DCP` → `source=dcp input_ma=1500 …`
  - `test_unplugged`: usb 0, tcpm 0 → `source=none input_ma=500 charge=on reason=source`
  - `test_no_tcpm_dir`: no tcpm directory, usb 1, ac 1 `USB_CDP` → `source=cdp input_ma=1500 …`
  - `test_hot`: temp 451 → `… input_ma=500 charge=off reason=hot`; `test_hot_hysteresis`: `decide` with
    `MU5358_PREV=off:hot` env and temp 420 → still `charge=off reason=hot`; temp 399 → `charge=on`
  - `test_garbage_temp`: temp file `abc` → `source=typec input_ma=500 charge=on reason=notemp`
  - `test_limit`: limit 80, capacity 80 → `charge=off reason=limit`; `MU5358_PREV=off:limit`, capacity 78 →
    off; 77 → on; limit `100` → never off; limit `20` (out of range) → ignored.
- [ ] **Step 2: Run** `python3 -m unittest tests.test_charge -v` → FAIL (no source).
- [ ] **Step 3: Write `tools/charge/mu5358-charge.c`** with the interfaces above, `main` handling `decide` only
  for now (`MU5358_PREV=on|off:reason` seeds `prev` for the tests). C99, no libraries beyond libc. A header
  comment states the register map used later and the hard cap.
- [ ] **Step 4: Run** → PASS. `tools/charge/build.sh`: as `tools/u30ui2/build.sh`, `aarch64-linux-gnu-gcc -O2
  -Wall -static -o mu5358-charge mu5358-charge.c`, prints `ls -la`. Build it once in docker
  (`docker run --rm -v "$PWD/tools":/t mu5358-mainline-build bash /t/charge/build.sh`); `file` says static aarch64.
- [ ] **Step 5: Commit** `git commit -m "mu5358-charge: the decision, from power-supply sysfs, with tests"`.

### Task 5: `mu5358-charge` — apply over I2C, the loop, the state file

**Files:**
- Modify: `tools/charge/mu5358-charge.c`
- Test: `tests/test_charge.py` (class `Apply`)

**Interfaces:**
- Produces: `int apply(const char *dev, int addr, const struct decision *d, struct applied *a)` where
  `struct applied { int reg00_before, reg00_after, reg01_before, reg01_after, rewritten; }`; the I2C device path
  from `MU5358_CHARGE_I2C` (default `/dev/i2c-6`) and address `0x6b`; for tests `MU5358_CHARGE_I2C` may name a
  regular file of 12 bytes (the register image): then `apply` reads/writes that file at offsets REG00/REG01 instead
  of ioctl (`S_ISREG` check), which is what the tests use.
- Register arithmetic: `REG00 = (reg00 & 0xE0) | ((input_ma - 100) / 100)` (500 → `0x04`, 1500 → `0x0e`),
  EN_HIZ (bit 7) cleared; `REG01 = charge ? reg01 | 0x10 : reg01 & ~0x10`. Write only when different; read back;
  a mismatch after the write → `rewritten=-1` and exit code 3 for `once`.
- CLI: `mu5358-charge once` = decide + apply + state line; `mu5358-charge` (no args) = the loop every 5 s
  (`MU5358_CHARGE_PERIOD` seconds for tests), prev kept across cycles, `rewrites` counted when `reg00_before`
  differs from what we last wrote; SIGTERM/SIGINT → `REG00 → 0x04` (IINLIM 500, HIZ 0), `REG01 |= 0x10`, state
  file removed, exit 0; `mu5358-charge status` prints the state file; `mu5358-charge limit [N]` reads or writes
  `/etc/mu5358/charge-limit` (50..100 or `off` = 100; others exit 2).
- State file `/run/mu5358/charge` (0644, written whole then renamed): one line
  `source=S input_ma=N charge=on|off reason=R temp=T capacity=C limit=L reg00=0xNN reg01=0xNN rewrites=K at=EPOCH`.

- [ ] **Step 1: Failing tests** (`class Apply`): a 12-byte register file seeded with Linux's bench values
  (`04 9a b4 a6 90 87 66 40 34 10 a0 00`):
  - `test_once_mac`: the Task 4 Mac tree + the file → stdout line has `input_ma=1500 charge=on reg00=0x0e
    reg01=0x9a rewrites=0`; the file's byte 0 is `0x0e`.
  - `test_hot_turns_charging_off`: temp 460 → byte 1 is `0x8a`, byte 0 `0x04`.
  - `test_rewrite_counted`: run `once` twice with `MU5358_STATE=<tmp>/state`, change byte 0 back to `0x04`
    between → second line `rewrites=1`.
  - `test_sigterm_restores`: start the loop with `MU5358_CHARGE_PERIOD=1`, wait for the state file, SIGTERM →
    exit 0, byte 0 `0x04`, byte 1 has bit 4 set, state file gone.
  - `test_limit_cli`: `limit 80` writes `80\n`; `limit 20` exits 2; `limit off` writes `100\n`; `limit` prints it.
  - `test_never_touches_other_bytes`: after any run bytes 2..11 unchanged.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** apply, the loop, the signal path, the state file, the CLI. ioctl path: `open(dev,
  O_RDWR)`, `ioctl(fd, I2C_SLAVE_FORCE, 0x6b)` (the vendor driver holds the address: `_FORCE` is required),
  `i2c_smbus_read_byte_data`/`write_byte_data` via `ioctl(I2C_SMBUS)` with `struct i2c_smbus_ioctl_data` (declare
  the structs locally from `<linux/i2c.h>`, `<linux/i2c-dev.h>`).
- [ ] **Step 4: Run** `python3 -m unittest tests.test_charge -v` → PASS; rebuild the static binary in docker.
- [ ] **Step 5: Commit** `git commit -m "mu5358-charge: IINLIM and CHG_CONFIG on the master bq2560x, loop, state file, limit"`.

### Task 6: Into the image and the release

**Files:**
- Modify: `openwrt/customize.sh` (near the u30ui2 copy at l.385; a new service block near `mu5358-thermal`),
  `tools/make-release.sh:34-35` (required files), `tools/ci-release.sh` (build line in "the tools"),
  `installer/mu5358-install.sh` (CARRY list: `etc/mu5358/charge-limit`), `README.md`, `docs/GKI-SYSTEM.md`
- Test: `tests/test_static.py` (class `Charge`)

**Interfaces:**
- Produces: `/opt/mu5358/bin/mu5358-charge` in the image; `/etc/init.d/mu5358-charge` START=98
  (`procd_set_param command /opt/mu5358/bin/mu5358-charge`, `respawn 3600 5 0`, enabled); the binary in the
  release's image, built by CI (`docker run --rm -v "$PWD/tools":/t mu5358-mainline-build bash /t/charge/build.sh`).

- [ ] **Step 1: Failing tests**:
```python
class Charge(unittest.TestCase):
    def test_in_image_and_release(self):
        c = (TOP / 'openwrt/customize.sh').read_text()
        self.assertIn('opt/mu5358/bin/mu5358-charge', c); self.assertIn('init.d/mu5358-charge', c)
        self.assertIn('respawn 3600 5 0', c[c.index('init.d/mu5358-charge'):][:800])
        self.assertIn('tools/charge/mu5358-charge', (TOP / 'tools/make-release.sh').read_text())
        self.assertIn('/t/charge/build.sh', (TOP / 'tools/ci-release.sh').read_text())
        self.assertIn('etc/mu5358/charge-limit', (TOP / 'installer/mu5358-install.sh').read_text())
```
- [ ] **Step 2: Run** → FAIL. **Step 3: Make the changes** (the init script after the `mu5358-thermal` pattern;
  the service waits up to 20 s for `/dev/i2c-6` before starting the daemon, in `start_service` via
  `procd_set_param command /bin/sh -c 'for i in $(seq 20); do [ -e /dev/i2c-6 ] && break; sleep 1; done; exec /opt/mu5358/bin/mu5358-charge'`).
  README: a "Charging" paragraph (what it does, `mu5358-charge status|limit`). `docs/GKI-SYSTEM.md`: the charger
  findings of 2026-11-01 (the registers, Android's 1.5 A, the ignored sysfs knobs) as a numbered finding.
- [ ] **Step 4: Run** the whole suite → OK; `sh -n` on the changed scripts. Commit:
  `git commit -m "mu5358-charge in the image: a procd service, carried charge limit, built by the release"`.

### Task 7: The bench — CI release, trial boot, the current

**Files:**
- Modify: `docs/GKI-SYSTEM.md` (the measurement), `docs/MU300-PARITY.md` (A5: charging done), `README.md`
  (status table: charging)

- [ ] **Step 1: CI** — dispatch Release; green (Tasks 1-3's gates); the Telegram message arrived.
- [ ] **Step 2: Slot a check** — `sh installer/mu5358-install.sh` path as on 2026-10-30: `adb -s 324761041479
  shell su -c 'sha256sum /dev/block/by-name/boot_a'` equals the backup's recorded hash (`backup/` notes); only then
  continue.
- [ ] **Step 3: Update the bench from Android** — download the prerelease's 8 files
  (`gh release download TAG --repo dikeckaan/mu5358-linux -D work/release/TAG`), push them, run
  `sh mu5358-install.sh run update` as on 2026-10-30 (settings carried), then `boot-linux` (the one-shot trial).
- [ ] **Step 4: Measure** over ssh `root@192.168.58.1` within 2 min of boot: `lsmod | grep i2c_dev`, `ls /dev/i2c-6`,
  `mu5358-charge status` → `input_ma=1500 charge=on`, `cat /sys/class/power_supply/battery/current_now` ≥ 1100000
  at capacity < 60 %, `grep 'Reg\[00\]' /sys/bus/i2c/devices/6-006b/registers | head -1` → `0x0e`, the slave's
  `Reg[00]` unchanged from before (`0x04` or `0x84`), `uname -r` unchanged, `dmesg | grep -ci "i2c.*error"` = 0.
- [ ] **Step 5: One hour** — a Monitor reading `mu5358-charge status` and battery temp every 5 min: temp never
  ≥ 450, `rewrites` count noted (the vendor-driver contention finding), capacity rising ≥ 10 points/h.
- [ ] **Step 6: Stop path** — `/etc/init.d/mu5358-charge stop` → `Reg[00]` `0x04` within 5 s; `start` → `0x0e`.
- [ ] **Step 7: Record** the numbers in `docs/GKI-SYSTEM.md`, A5's charge half in `docs/MU300-PARITY.md`, README's
  status row. Commit: `git commit -m "docs: charging at 1.5 A on the bench (mu5358-charge), the first CI release"`.
  Push; the Tests workflow green.
