# The safe package from kanoqwq/u30pro-bringup: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** NFC tap-to-connect for the hotspot, USB host mode through the Type-C port's `port_type`, a host
simulator and fuzzer for u30ui2, charger-event wake of the screen, and a power-key menu, on the system as it
ships today (GKI 6.6, OpenWrt tree, u30ui2, LuCI panel).

**Architecture:** one static C tool talks to the NFC tag over i2c-dev; one shell script owns the tag's
content and is called by events (procd reload, ifup/ifdown, the two `wifi reload` writers, the switches);
the screen and the panel only flip a state file and read a result file. USB host is a change inside the
existing `mu5358-usb` and its rpcd object. The simulator compiles the real `ui.c` with stubs and a virtual
clock, reads events from stdin and writes PNGs; the fuzzer feeds it random events under ASan/UBSan. The
uevent subscription and the `power` command are small additions to `ui.c`'s poll loop; `buttons` picks its
long-press threshold by whether the dashboard runs.

**Tech Stack:** C11 (static aarch64 in `mu5358-mainline-build`; host gcc with ASan in the same image), POSIX sh
(busybox), procd/hotplug, rpcd shell plugins, LuCI JS, Python unittest (docker busybox for shell, docker
`mu5358-mainline-build` for host C), `tools/u30ui2/mkfont.sh` for the CJK subset.

**Spec:** `docs/superpowers/specs/2026-11-03-kano-safe-package-design.md`

## Global Constraints

- Never slot a, the NV partitions, the GPT, misc from Linux; Linux stays a one-shot trial; no SIM data; never
  read the VPN config; never `clk_summary`; secrets never in the repository, on a command line, in a log or a
  transcript; the bench password never printed.
- The Wi-Fi key reaches `mu5358-nfc` on stdin only; the tool prints at most `key N characters`; `show` prints
  the SSID only with `-v`; the bench never prints the device's SSID or `wireless.ap0`.
- Any bench step that can cut the Mac's USB link arms first:
  `( trap "" HUP; sleep 300; [ -e /run/mu5358/usb-keepN ] || reboot ) </dev/null >/dev/null 2>&1 &` and the
  undo file is created only after the link is back (`N` = a fresh number per trial).
- Screen strings only through `tools/u30ui2/i18n.h` (EN, TR, ZH) and `mkfont.sh` after any non-ASCII change;
  panel strings in `openwrt/panel/po/{tr,zh_Hans}/mu5358.po`.
- New binaries static aarch64, built by a `build.sh` in their directory in `mu5358-mainline-build` (pattern:
  `tools/u30ui2/build.sh`), listed in `tools/make-release.sh`'s file check (line 34), shipped by
  `openwrt/customize.sh`; the release keeps 11 assets (`tools/ci-release.sh` line 136).
- Tests: `cd tests && TMPDIR=/Volumes/TasinabilirSSD/projects/mu5358-linux/work python3 -m unittest discover
  -s . -p 'test_*.py'` and `node --test tests/web/*.mjs` stay green; shell under docker busybox
  (`tests/helpers.py`: `DOCKER`, `BUSYBOX_IMAGE`, `docker_busybox()`), host C under docker
  `mu5358-mainline-build`; a missing docker skips with a reason, never passes silently.
- Settings in `/etc/mu5358/<name>`; the installer's `CARRY` (installer/mu5358-install.sh:611) names every
  new one; rpcd plugins allow-list every argument (pattern `openwrt/usb/rpcd-mu5358usb`) and ship an ACL.
- File headers of ported code say: from `kanoqwq/mu300-linux` `u30pro-bringup` (origin `33333s/U30pro-openwrt`),
  MIT.
- Commit after every task with the session's attribution lines.

## Review Focus

1. An SSID with spaces, quotes, `$`, or UTF-8 up to 32 bytes, and a key with `$`, backquotes, a leading `-`,
   a trailing CR/LF: the tag gets the exact bytes, the key never appears in any argv (Task 2, `test_key_only_on_stdin`,
   `test_ssid_bytes_exact`).
2. The tag disappears or the bus errors in the middle of a write: the tag never holds a half credential (the
   empty-length page is committed first), the result is `error`, the next sync repairs it (Task 1,
   `test_interrupted_write_leaves_empty_ndef`).
3. A flood of `battery` uevents (the charger manager's log noise) neither wakes the screen nor starves touch
   input (Task 8, `test_battery_flood_ignored`, `test_flood_keeps_input`).
4. The power key held with the screen off and the UI locked opens the menu; nothing but the menu's own hold
   powers the device off (Task 9, `test_power_cmd_wakes_locked`, `test_long_press_no_ui_powers_off`).
5. `port_type` not writable (an older kernel) or `musb_hostenable` absent: `host` falls back to the data-role
   swap, `status` still answers (Task 6, `test_host_falls_back_to_data_role`, `test_status_without_musb_attr`).

---

## File structure

| Path | Responsibility |
| --- | --- |
| `tools/nfc/mu5358-nfc.c`, `tools/nfc/build.sh` | the tag tool (ported), static build |
| `tests/fixtures/nfc_i2c.c` | file-backed `/dev/i2c-4` stand-in: `open`/`ioctl` replaced, the tool compiled against it |
| `openwrt/nfc/mu5358-nfc-sync` | the one writer of the tag: state file, hotspot → record, triggers |
| `openwrt/nfc/mu5358-nfc.init`, `openwrt/nfc/95-mu5358-nfc.hotplug` | S95 one-shot + reload trigger; ifup/ifdown of the AP |
| `openwrt/nfc/rpcd-mu5358nfc`, `openwrt/nfc/acl-mu5358nfc.json`, `openwrt/nfc/nfc.js` | the panel's card |
| `openwrt/customize.sh` | ships all of it; `buttons` threshold + sync call; card injection |
| `openwrt/usb/mu5358-usb`, `openwrt/usb/rpcd-mu5358usb`, `openwrt/usb/usb-role.js` | host mode by `port_type`; the role card |
| `tools/u30ui2/ui.c`, `data.c`, `data.h`, `i18n.h` | NFC row, uevent wake, `power` command, `UI_HOST` seams |
| `tools/u30ui2/sim.c`, `tools/u30ui2/build.sh` | the simulator (stubs + virtual clock + command language) |
| `tools/u30ui/display.c` | first frame before the mode-set |
| `tests/test_nfc.py`, `tests/test_u30ui2.py`, `tests/test_usb.py`, `tests/test_static.py` | the tests |
| `docs/GKI-SYSTEM.md`, `README.md`, `docs/MU300-PARITY.md` | the record |

---

### Task 1: The NFC tool and its file-backed test bench

**Files:**
- Create: `tools/nfc/mu5358-nfc.c` (from the fork's `devices/u30pro/native/tools/u30pro_nfc.c`, 362 lines),
  `tools/nfc/build.sh`, `tests/fixtures/nfc_i2c.c`
- Test: `tests/test_nfc.py` (class `Tool`)

**Interfaces:**
- Produces: binary `mu5358-nfc` with `show [-v] | clear | wifi <ssid> open|wpa2 (key on stdin) | encode <ssid> open|wpa2 [key]`;
  exit 0 ok, 1 tag/bus error, 2 usage/bad key; stdout `written` or `unchanged` after `wifi`/`clear`;
  `show` prints `tag /dev/i2c-4 0x57, data area N bytes, SAK 0x00`, then `ssid N bytes` (`-v`: the SSID),
  `auth 0x0020|0x0001`, `key N characters`, or `empty NDEF message` / `no NDEF message` / `not a Wi-Fi credential`.
- Produces: `tests/test_nfc.py::NFC_IMAGE` fixture convention: env `NFC_TEST_IMAGE=<file>` (a 1 KiB file: bytes
  0x00-0x0B UID/lock, CC at 0x0C = `e1 10 6d 00`, data from 0x10, SAK at 0x3BF), `NFC_TEST_FAIL_AT=<addr>`
  makes the stand-in fail every write at that address (for the interrupted write).

- [ ] **Step 1: Write the failing tests** in `tests/test_nfc.py`: a `Tool` class that compiles
  `tests/fixtures/nfc_i2c.c` + `tools/nfc/mu5358-nfc.c` once in docker `mu5358-mainline-build`
  (`gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -include tests/fixtures/nfc_i2c.h`
  — decide: the fixture defines `open`/`ioctl` wrappers through `-Dopen=test_open -Dioctl=test_ioctl` on the
  tool's translation unit, as the fork's `u30pro_nfc_io.c` does; skip with the docker reason when unavailable),
  with an **independent** WSC encoder `credential(ssid: bytes, key: bytes = b'') -> bytes` written in the test
  (TLVs 0x1026, 0x1045, 0x1003 = `00 20`/`00 01`, 0x100f = `00 08`/`00 01`, 0x1027, 0x1020 = ff×6; outer
  0x104a = `10`, 0x100e, 0x1049 = `00 37 2a 00 01 20`; NDEF record `d2 <typelen> <paylen> application/vnd.wfa.wsc`;
  TLV `03 <len> ... fe`), and:
  - `test_encode_matches_oracle`: `encode MyNet wpa2 secretpass1` hex == `credential(b'MyNet', b'secretpass1').hex()`; open too.
  - `test_wifi_writes_and_reads_back`: `wifi MyNet wpa2` with the key on stdin → stdout `written`, the image's
    data area from 0x10 equals the oracle; the tool's argv never contained the key (the test passes it only via stdin).
  - `test_second_write_unchanged`: same again → `unchanged`, image bytes identical.
  - `test_clear`: → data area starts `03 00 fe`; `show` says `empty NDEF message`.
  - `test_show_masks_ssid`: after a write, `show` output contains `ssid 5 bytes` and not `MyNet`; `show -v` contains `MyNet`; neither contains the key.
  - `test_readonly_cc_refused`: CC byte 3 = `0x0f` → `wifi` exits 1, image unchanged, stderr `not a writable NDEF tag`.
  - `test_bad_key_rejected`: 7 characters → exit 2; 64 non-hex → exit 2; 63 → ok.
  - `test_ssid_bytes_exact`: SSID `b'Caf\xc3\xa9 "x" $y'` (as one argv) is stored byte for byte; 33 bytes → exit 2.
  - `test_interrupted_write_leaves_empty_ndef`: `NFC_TEST_FAIL_AT=0x18` → exit 1, the data area's byte at 0x11 (NDEF length) is 0.
  - `test_sak_rewritten_when_not_type2`: SAK byte 0x20 → after `wifi`, SAK is 0x00 and the stand-in saw writes to 0xfff4 and 0xffe6 (the fixture records register writes in `NFC_TEST_IMAGE.log`).
- [ ] **Step 2: Run** `cd tests && TMPDIR=... python3 -m unittest test_nfc -v` — Expected: errors, `tools/nfc/mu5358-nfc.c` missing.
- [ ] **Step 3: Port the tool**: copy the fork's file, rename `u30pro-nfc` → `mu5358-nfc` in messages and usage,
  keep BUS/ADDR/CC/SAK/DATA, add `-v` to `show` (default prints `ssid %u bytes`), header credits the origin.
  Write `tests/fixtures/nfc_i2c.c`: `test_open` (only `/dev/i2c-4` → opens `NFC_TEST_IMAGE`), `test_ioctl`
  (`I2C_RDWR`: 16-bit address from the first message; reads copy from the file; writes of 2+n bytes with
  address < 0x400 go to the file unless `NFC_TEST_FAIL_AT` matches; addresses 0xfff4/0xffe6 are appended to
  the `.log`; everything else `EIO`). Write `tools/nfc/build.sh` (docker `mu5358-mainline-build`,
  `aarch64-linux-gnu-gcc -O2 -Wall -static`, strip, output `tools/nfc/mu5358-nfc`; `.gitignore` the binary).
- [ ] **Step 4: Run the tests** — Expected: `Tool` all PASS.
- [ ] **Step 5: Build the device binary** `sh tools/nfc/build.sh`; `file tools/nfc/mu5358-nfc` says static aarch64.
- [ ] **Step 6: Commit** `tools/nfc tests/fixtures/nfc_i2c.c tests/test_nfc.py .gitignore` — "nfc: the tag tool (from kanoqwq/u30pro-bringup) with a file-backed i2c test bench".

### Task 2: The sync script, its service and hotplug

**Files:**
- Create: `openwrt/nfc/mu5358-nfc-sync`, `openwrt/nfc/mu5358-nfc.init`, `openwrt/nfc/95-mu5358-nfc.hotplug`
- Test: `tests/test_nfc.py` (class `Sync`, docker busybox like `tests/test_update.py`'s `Updater`)

**Interfaces:**
- Produces: `mu5358-nfc-sync [sync|on|off|toggle|state|supported]`; env overrides `MU5358_NFC_TOOL`
  (default `/opt/mu5358/bin/mu5358-nfc`), `MU5358_NFC_STATE` (`/etc/mu5358/nfc`), `MU5358_NFC_RUN`
  (`/run/mu5358`), `MU5358_UCI` (`uci`). `state` prints `on|off`; `supported` exits 0 when the tool is
  executable and `/dev/i2c-4` exists (`MU5358_NFC_BUS` override). Result file `$RUN/nfc-result`: one of
  `open wpa2 disabled hotspot_off unsupported no_tag error`. Lock `$RUN/nfc.lock` (busybox `flock`).
- Consumes: Task 1's tool interface.

- [ ] **Step 1: Write the failing tests** (`Sync`): a busybox container with `/w` read-only, a `uci` stub that
  answers from a key=value file (`wireless.ap0.ssid`, `.key`, `.encryption`, `.disabled`), an `mu5358-nfc`
  stub that records argv to `argv.log` and stdin to `stdin.bin` and answers `written`:
  - `test_default_on_writes_wpa2`: no state file, `encryption=psk2` → stub called `wifi <ssid> wpa2`, `stdin.bin` == key bytes exactly, result `wpa2`.
  - `test_key_only_on_stdin`: key `pa$s\`w-ord\r\n`-like value (`-start $x \`y\``) never in `argv.log`; stdin has it byte for byte (no trailing newline added by the script beyond what uci returned — pin: `printf '%s'`).
  - `test_off_clears`: state `off` → `clear`, result `disabled`.
  - `test_hotspot_disabled_clears`: `disabled=1` → `clear`, result `hotspot_off`.
  - `test_unsupported_encryption_clears`: `sae`, `psk`, `owe`, `wpa2+eap` each → `clear`, result `unsupported`; `none` → `wifi <ssid> open`; `sae-mixed`, `psk2+ccmp` → `wpa2`.
  - `test_toggle_on_off_state`: `toggle` writes `off`, then `on`; `state` follows; `on` removes the file (absent = on).
  - `test_tool_failure_is_error`: stub exits 1 → result `error`, exit 1, the stub's stderr file under `$RUN` has mode 0600.
  - `test_no_tool_is_no_tag`: `MU5358_NFC_TOOL` missing → result `no_tag`, exit 0, no stub call.
  - `test_lock_serialises`: two syncs started together, the stub sleeps 1 s: `argv.log` shows them 1 s apart (sequential).
  - `test_init_and_hotplug_shape`: the init file has `START=95`, `procd_add_reload_trigger wireless`, no `respawn`; the hotplug script runs `sync` only for `ACTION` ifup/ifdown of `INTERFACE` named by `uci get wireless.ap0.network` (default `lan`) — decide: the AP's `network` option; pin `ifup`/`ifdown` on that interface, nothing on others.
- [ ] **Step 2: Run** `python3 -m unittest test_nfc.Sync -v` — Expected: FAIL, script missing.
- [ ] **Step 3: Write the three files** (sh, `set -u`, the fork's `u30pro-nfc-control` as the model: the
  `--locked` re-exec under `flock -x`, `fail()` writing `error`, the key in a variable piped with `printf '%s'`
  and unset after; `logger -t mu5358-nfc` one line per failure without the SSID).
- [ ] **Step 4: Run the tests** — Expected: PASS.
- [ ] **Step 5: Commit** — "nfc: mu5358-nfc-sync, the tag's one writer, with its service and hotplug".

### Task 3: Shipping, carry, the two `wifi reload` writers, release

**Files:**
- Modify: `openwrt/customize.sh` (ship the tool, the script, the init as `S95mu5358-nfc`, the hotplug; `buttons`
  heredoc: `wifi reload` followed by `/opt/mu5358/bin/mu5358-nfc-sync sync`), `installer/mu5358-install.sh:611`
  (`CARRY` += `etc/mu5358/nfc`), `tools/make-release.sh:34` (+ `tools/nfc/mu5358-nfc`), `tools/u30ui2/ui.c:751,921`
  (the two `data_run` strings gain `; /opt/mu5358/bin/mu5358-nfc-sync sync`)
- Test: `tests/test_static.py` (class `Nfc`), `tests/test_static.py:555` (`test_carry` names `etc/mu5358/nfc`)

- [ ] **Step 1: Write the failing tests**: `test_shipped` (customize.sh copies `openwrt/nfc/mu5358-nfc-sync` to
  `/opt/mu5358/bin/`, the init to `/etc/init.d/mu5358-nfc` with `S95mu5358-nfc`, the hotplug to
  `/etc/hotplug.d/iface/95-mu5358-nfc`, the binary to `/opt/mu5358/bin/mu5358-nfc`, and fails loudly when the
  binary is missing — same shape as the `usb-watch` line), `test_every_wifi_reload_syncs` (every occurrence of
  `wifi reload` in `openwrt/customize.sh`, `tools/u30ui2/ui.c`, `tools/u30ui/ui.c` is followed on the same line
  or the next statement by `mu5358-nfc-sync sync`), `test_release_lists_tool`, `test_carry` extended.
- [ ] **Step 2: Run** — Expected: FAIL.
- [ ] **Step 3: Make the changes.** `tools/u30ui/ui.c:672` gets the same suffix (the classic UI toggles the hotspot too).
- [ ] **Step 4: Run** `python3 -m unittest test_static -v` — Expected: PASS; the whole suite green.
- [ ] **Step 5: Commit** — "nfc: shipped by customize.sh, carried by the installer, synced after every wifi reload".

### Task 4: The screen's NFC row

**Files:**
- Modify: `tools/u30ui2/i18n.h` (+ `S_NFC` "NFC tap-to-connect"/"NFC ile bağlan"/"NFC 连接", `S_UNSUPPORTED`
  "Unsupported"/"Desteklenmiyor"/"不支持", `S_NFC_WRITTEN` "NFC: hotspot written"/"NFC: hotspot yazıldı"/"NFC 已写入",
  `S_NFC_FAILED` "NFC failed"/"NFC başarısız"/"NFC 失败"), `tools/u30ui2/data.h` (`int nfc; /* 0 off, 1 on, 2 unsupported */`,
  `char nfc_result[16];`), `tools/u30ui2/data.c` (`collect_slow`: `/etc/mu5358/nfc`, `/run/mu5358/nfc-result`,
  `supported` = `/opt/mu5358/bin/mu5358-nfc` executable), `tools/u30ui2/ui.c` (`draw_wifi`: a row at y=142
  `NFC: On|Off|Unsupported` between the clients line and the password row; `P_WIFI` tap `130 <= y < 154` →
  `data_run("/opt/mu5358/bin/mu5358-nfc-sync toggle")` when supported; the toast after the result file changes
  is Task 7's simulator territory — here only the row), `tools/u30ui2/font_cjk.h` (regenerated)
- Test: `tests/test_static.py::Nfc::test_screen_row` (strings present in `i18n.h` with three columns, the two
  data fields, the toggle command string, `font_cjk.h` newer than `i18n.h` in git — pin: the test runs
  `mkfont.sh`'s character extraction and asserts every CJK char of `i18n.h` is in `font_cjk.h`'s table; read
  `mkfont.sh` for the table format)

- [ ] **Step 1: Write the failing test.** — [ ] **Step 2: Run, FAIL.** — [ ] **Step 3: Implement; run
  `tools/u30ui2/mkfont.sh`; build with `docker run --rm -v "$PWD/tools":/t mu5358-mainline-build bash /t/u30ui2/build.sh`
  and `build.sh preview` (the Wi-Fi page PNG shows the row: look at it).** — [ ] **Step 4: Run, PASS.**
- [ ] **Step 5: Commit** — "u30ui2: NFC row on the Wi-Fi page".

### Task 5: The panel's NFC card

**Files:**
- Create: `openwrt/nfc/rpcd-mu5358nfc` (`list` → `{"get":{},"set":{"value":"str"}}`; `get` →
  `{"ok":1,"state":"on|off","supported":0|1,"result":"<result word>"}`; `set {"value":"on|off"}` →
  `mu5358-nfc-sync on|off`, `{"ok":1}` or `{"ok":0,"error":"nfc_value"}`; env `MU5358_NFC_SYNC`),
  `openwrt/nfc/acl-mu5358nfc.json` (as `acl-mu5358usb.json`), `openwrt/nfc/nfc.js` (card `NFC` on System >
  Device: rows State, Tag (`hotspot credential` / `empty` / the reason words translated), buttons On/Off; no SSID shown)
- Modify: `openwrt/customize.sh` (copy the three; inject the card into `device.js` next to the USB disk card,
  same `sed` pattern, marker `mu5358: NFC card`), `openwrt/panel/po/tr/mu5358.po`, `openwrt/panel/po/zh_Hans/mu5358.po`
  (`NFC`, `NFC tap-to-connect`, `Tag`, `State`, `hotspot credential`, `empty`, `Hotspot off`, `Unsupported encryption`, `No tag`, `Failed`, `Turn on`, `Turn off` where missing)
- Test: `tests/test_nfc.py::Rpcd` (pattern `tests/test_usb.py::Rpcd`: `list` shape, `get` fields from a stub
  `state`/`supported`/result file, `set on|off` calls the stub with exactly that word, `set x` → `nfc_value`,
  a value with `;` never reaches the stub), `tests/test_static.py::Nfc::test_panel_card` (files shipped,
  injection marker, ACL object name, po entries present in both catalogs)

- [ ] **Step 1: Write the failing tests.** — [ ] **Step 2: Run, FAIL.** — [ ] **Step 3: Implement.**
- [ ] **Step 4: Run; the whole suite and `node --test tests/web/*.mjs` — PASS.**
- [ ] **Step 5: Commit** — "panel: the NFC card (rpcd mu5358nfc)".

### Task 6: USB host mode through `port_type`

**Files:**
- Modify: `openwrt/usb/mu5358-usb:266-290` (`role host|device|auto`: `auto` new; `host` → `port_type`
  `source` when `$TYPEC/port_type` is writable, else the data-role swap; `device` → `sink`; the self-undo writes
  `dual` (or `device` to `data_role` on the fallback path); `status` adds `policy: auto|host|device` from
  `port_type` and `host capable: yes|no` from `$MUSB/musb_hostenable` with `MUSB=${MU5358_MUSB:-$(ls -d
  /sys/bus/platform/drivers/musb-sprd/*.usb 2>/dev/null | head -n 1)}`), `openwrt/usb/rpcd-mu5358usb` (`get` +
  `policy`, `host_capable`; `set kind=role value=auto|host|device`), `openwrt/customize.sh:602` (the role card
  injected instead of hidden), `openwrt/panel/po/*` (strings)
- Create: `openwrt/usb/usb-role.js` (card `USB role`: policy, data role, host capable, three buttons, the
  sentence "Host mode undoes itself after 60 s unless kept; changing the role may replug the USB link for a moment")
- Test: `tests/test_usb.py` (class `Role`, extend or add): `test_auto_writes_dual`, `test_host_prefers_port_type`,
  `test_device_writes_sink`, `test_host_falls_back_to_data_role` (port_type read-only in the fixture),
  `test_status_without_musb_attr` (`MU5358_MUSB` empty dir → `host capable: no`), `test_undo_restores_dual`
  (`MU5358_HOST_UNDO=1`, sleep 2, `dual` written), `Rpcd::test_role_set_and_get`; `tests/test_static.py` for the card and po.

- [ ] **Step 1: Write the failing tests.** — [ ] **Step 2: Run, FAIL.** — [ ] **Step 3: Implement.**
- [ ] **Step 4: Run — PASS; suite green.**
- [ ] **Step 5: Bench, guarded** (each trial: a fresh `usb-keepN`, the reboot guard, then the command over ssh
  with the output to `/tmp/usbN.log`, wait 90 s, ssh back, create the keep file, read the log): `status`;
  `auto`; `device`; `host`. Record in the task's ledger which ones dropped the link and whether it returned by
  the undo; put the sentence into the card/tool only for those that did. The OTG adapter test is the
  maintainer's: write that into `docs/GKI-SYSTEM.md`'s USB paragraph in Task 11.
- [ ] **Step 6: Commit** — "usb: host mode by the Type-C port_type policy, the USB role card".

### Task 7: The u30ui2 simulator

**Files:**
- Create: `tools/u30ui2/sim.c` (host: stubs for display/input/data as `preview.c`, plus `sim_now_ms` virtual
  clock, a recorded command list `sim_commands[]` filled by `data_run`, the stdin reader and the command
  language, `shot NAME` → `<outdir>/NAME.png` via `png_write`, `state` → one line
  `page=<name> screen=on|off locked=0|1 toast=<text or ->`), `tools/u30ui2/build.sh` (`sim` mode: gcc, `-DUI_HOST`,
  output `/tmp/u30ui2-sim` or `$2`)
- Modify: `tools/u30ui2/ui.c` (seams: `now_ms()`/`poll()`/signals/`input_*` are the device's unless `UI_HOST`;
  `main()` becomes `ui_main(void)` with the loop body callable once per injected event: factor the loop body
  into `static void ui_step(long now)` and the event dispatch into `ui_event(struct gesture*)`; the existing
  `/tmp/u30ui.cmd` parser moves into `static int ui_command(const char *line)` shared by the device hook and the simulator)
- Test: `tests/test_u30ui2.py` (class `Sim`): builds once in docker `mu5358-mainline-build` (`build.sh sim`),
  helper `run(script: str) -> (stdout, pngs)`; `png_nonblank(path)` decodes the PNG with `zlib` (the writer in
  `tools/u30ui/png.c` — read it for the filter/IHDR it emits) and asserts > 2 % non-background pixels:
  - `test_every_page_every_language_renders`: for lang in en/tr/zh, for each page name: `page X`, `shot`, non-blank.
  - `test_swipe_moves_and_settles`: `swipe 200 120 60 120 240`, `wait 400`, `state` says the next page; a swipe back returns.
  - `test_swipe_down_goes_home`.
  - `test_settings_rows_cycle`: taps on the six rows change `state`/the drawn value (assert the recorded command for the USB row is `/opt/mu5358/bin/mu5358-usb storage on`).
  - `test_power_hold_runs_action`: `page power`, `down 120 72`, `wait 1100`, `up` → `sim_commands` has `sync; reboot`; a 500 ms hold runs nothing.
  - `test_idle_timeout_turns_off_and_locks`: `set timeout 30`, `set lock hold`, `wait 31000` → `screen=off locked=1`.
  - `test_nfc_row_toggles`: `page wifi`, `set nfc 1`, `tap 120 142` → command `/opt/mu5358/bin/mu5358-nfc-sync toggle`; `set nfc 2` → no command.
  - `test_unknown_command_fails`: exit status 2.
- [ ] **Step 1: Write the failing tests.** — [ ] **Step 2: Run, FAIL (no sim.c).** — [ ] **Step 3: Implement**
  (the seams first, the device binary rebuilt and byte-compared in behaviour by the existing preview; then sim.c).
- [ ] **Step 4: Run — PASS; rebuild the device binary; suite green.**
- [ ] **Step 5: Commit** — "u30ui2: a host simulator with a scripted event language".

### Task 8: Event wake and the fuzzer

**Files:**
- Modify: `tools/u30ui2/ui.c` (`power_events_open()`, `power_events_read()`, `power_uevent(const char *buf, size_t n)`
  → `int` changed; the netlink fd in the poll set; filter `SUBSYSTEM=power_supply`, `ACTION=change`,
  `POWER_SUPPLY_NAME` in {`ac`, `usb`, `sprd-tcpm-source-psy-sc27xx-pd`}, `POWER_SUPPLY_ONLINE` 0|1 differing from the
  last value of that supply → `wake()`; `battery` and any other name ignored; baseline from
  `/sys/class/power_supply/<name>/online` after the bind; `ENOBUFS` → one baseline re-read), `tools/u30ui2/sim.c`
  (`uevent NAME 0|1` builds the packet `change@/devices/x\0ACTION=change\0SUBSYSTEM=power_supply\0POWER_SUPPLY_NAME=NAME\0POWER_SUPPLY_ONLINE=N\0`
  and calls `power_uevent`), `tests/test_u30ui2.py` (class `Fuzz`)
- Test: `tests/test_u30ui2.py::Sim`: `test_uevent_ac_wakes` (`screen off`, `uevent ac 1` → `screen=on`, `locked` unchanged),
  `test_uevent_same_value_no_wake` (`uevent ac 1` twice: the second with `screen off` before it does not wake),
  `test_battery_flood_ignored` (1000 × `uevent battery 1/0` with the screen off → still off),
  `test_flood_keeps_input` (the same flood then `tap` on the settings row within the same run changes state);
  `Fuzz::test_seeded_fuzz`: `build.sh sim-asan` (`-O1 -g -fsanitize=address,undefined`), 4000 events from
  `random.Random(seed)` over tap/swipe/hold/key/wait/page/lang/set/uevent/screen (the fork's `fuzz.sh` mix),
  seed 1 fixed and `MU5358_FUZZ_SEED` added when set; the run must exit 0 with an empty stderr.
- [ ] **Step 1: Write the failing tests.** — [ ] **Step 2: Run, FAIL.** — [ ] **Step 3: Implement.** — [ ] **Step 4: Run, PASS.**
- [ ] **Step 5: Bench**: deploy the new `u30ui2` (ssh cat copy, `/etc/init.d/mu5358-screen restart`), install a
  temporary `/etc/hotplug.d/power_supply/99-probe` that appends `$ACTION $POWER_SUPPLY_NAME $POWER_SUPPLY_ONLINE`
  to `/tmp/psprobe.log`, then ask the maintainer to unplug and replug the cable once; read the log, write
  which names fired into the ledger and `docs/GKI-SYSTEM.md`; remove the probe. If none of the three fired, the
  filter gains the name that did (never `battery`), with a test.
- [ ] **Step 6: Commit** — "u30ui2: wake on the charger's uevent; seeded fuzz under ASan".

### Task 9: The power key opens the menu

**Files:**
- Modify: `openwrt/customize.sh:354-413` (`buttons`: `ui=$(cat /etc/mu5358/ui)`; `dash=0`; `[ "$ui" != classic ] && [ -x /opt/mu5358/bin/u30ui2 ] && dash=1`;
  `mu5358-keys -l $([ $dash = 1 ] && echo 1500 || echo 3000)`; `"116 long"`: `if [ $dash = 1 ] && p=$(pidof u30ui2); then echo power > /tmp/u30ui.cmd; kill -USR2 $p; else poweroff; fi`;
  `mu5358-screen`'s `start_service` ends with `/etc/init.d/mu5358-buttons restart` — or a `service_started` hook: pick the former),
  `tools/u30ui2/ui.c` (`ui_command("power")`: `display_backlight(1)`, `page = P_POWER`, `last_touch = now`, redraw; allowed while `locked`)
- Test: `tests/test_static.py::Buttons` (extract the `buttons` heredoc between its `cat > "$R/opt/mu5358/bin/buttons" <<'EOF'` and `EOF`,
  run under docker busybox with a stub `mu5358-keys` that prints its `-l` and then the lines given by the test, a
  stub `pidof`, `kill` recorded, `poweroff` recorded): `test_dashboard_threshold_1500`, `test_classic_threshold_3000`,
  `test_long_press_sends_power_cmd` (file content `power`, `kill -USR2 <pid>`), `test_long_press_no_ui_powers_off`
  (pidof empty → `poweroff` recorded, no cmd file), `test_short_press_toggles_backlight` unchanged;
  `tests/test_u30ui2.py::Sim::test_power_cmd_wakes_locked` (`set lock hold`, `screen off`, `cmd power` → `page=power screen=on locked=1`; a `tap` on the home area stays blocked).
- [ ] **Steps 1-4 as above.** — [ ] **Step 5: Bench**: deploy `buttons` + the binary; hold the power key 1.5 s
  (the maintainer's finger, or `/tmp/u30ui.cmd` + SIGUSR2 from ssh to prove the UI side): the Power page shows; no power-off.
- [ ] **Step 6: Commit** — "buttons: the power key opens the dashboard's Power page".

### Task 10: The first frame before the mode-set

**Files:**
- Modify: `tools/u30ui/display.c:91-200` (`int display_init(void (*first)(void))`: the buffers are mapped,
  `first()` draws into buffer 0 (gfx bound to it), then `panel_turn()` is tried before `SETCRTC` and `SETCRTC`
  shows buffer 0), `tools/u30ui/ui.c`, `tools/u30ui2/ui.c`, `tools/u30ui2/preview.c`, `tools/u30ui2/sim.c`
  (callers pass a first-draw function or `NULL`)
- Test: `tests/test_static.py::Screen::test_display_init_takes_first_frame` (signature in `display.h`, every
  caller passes a function, `SETCRTC` appears after the `first(` call in `display.c`).
- [ ] **Steps 1-4.** — [ ] **Step 5: Bench**: `/etc/init.d/mu5358-screen restart` three times while watching
  the panel: no black frame and no flipped frame; if a flipped frame shows, `panel_turn` returns to after
  `SETCRTC` and the ledger records "measured: flipped; status quo kept". — [ ] **Step 6: Commit.**

### Task 11: The record

**Files:**
- Modify: `docs/GKI-SYSTEM.md` (services table: `mu5358-nfc` S95; the Screen row: NFC row, event wake, power
  menu; a "NFC tap-to-connect" paragraph with the tag facts: i2c-4 0x57, CC `e1 10 6d 00`, SAK 0; the USB
  paragraph: `port_type`, `musb_hostenable`, what the guarded trials showed, the OTG test still the maintainer's;
  the uevent probe's result), `README.md` (features list: NFC, USB role), `docs/MU300-PARITY.md` (a line: the
  kano/33333s safe package, 2026-11-03), the fork credit in `docs/GKI-SYSTEM.md`'s sources list.
- [ ] **Step 1: Write.** — [ ] **Step 2: `python3 -m unittest test_static` (docs tests) PASS.** — [ ] **Step 3: Commit.**

### Task 12: Release and bench of the whole package

- [ ] **Step 1:** `gh workflow run release.yml --repo dikeckaan/mu5358-linux --ref main`; `gh run watch` in the background; 11 assets.
- [ ] **Step 2: Bench update from Linux** (the W2 path, served from the Mac: `python3 -m http.server 8000 --bind 192.168.58.200`,
  `MU5358_RELEASE_URL`/`MU5358_API_URL`, the `ping` stub, `MU5358_SELF_UPDATED=1`): apply, the restart to Android, `boot-linux`.
- [ ] **Step 3: On the new system:** `mu5358-nfc show` (no `-v`; SSID masked by the tool), `/run/mu5358/nfc-result`
  is `wpa2` or `open`; `mu5358-nfc-sync off` → `show` says `empty NDEF message`; `on` → written again; the
  screen's row and the panel's card show the same; `mu5358-usb status` has `policy:` and `host capable:`;
  `logread -e mu5358-nfc` has no SSID or key. The phone tap and the OTG adapter are the maintainer's.
- [ ] **Step 4:** report; the maintainer promotes the release.
