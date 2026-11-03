# The safe package from kanoqwq/u30pro-bringup: NFC, USB host, the screen's test bench, event wake, power menu

Design of 2026-11-03. The first of four work packages the maintainer chose from the review of
`kanoqwq/mu300-linux` branch `u30pro-bringup` (its U30 Pro hardware port came from `33333s/U30pro-openwrt`,
MIT, no longer public); the order agreed: this package, then the SIM topology, then the modem without the
Android chroot, then the mainline 6.12 kernel (W7). This package touches neither the kernel nor the modem.

## Intent

Take the parts of the fork that make the device nicer to use or the project safer to change, port them onto
this system (GKI 6.6 kernel, OpenWrt tree, u30ui2 on the screen, LuCI panel), and do it with the rigour of
the W2 work: every behaviour pinned by a test that ran, every bench step with a self-undo, nothing claimed
that was not seen.

Why these five and not the rest: they are userspace, they run on the kernel we ship today, and their
failure modes are small (a tag that does not update, a USB role that undoes itself, a test that fails).

## Goals

1. **NFC tap-to-connect.** The U30 Pro's NFC tag (Fudan FM11NT08x on `/dev/i2c-4` at 0x57, confirmed on the
   bench unit: capability container `e1 10 6d 00`, SAK `0x00`, no kernel driver bound to the address; the
   stock `tmsnfc` module only holds the reset and interrupt lines) carries the hotspot's Wi-Fi credential as a
   Wi-Fi Simple Configuration NDEF record, so a phone that touches the device joins the hotspot. The record
   follows the hotspot: SSID, key and encryption changes, hotspot on/off, and a boot all re-sync it. A
   switch on the screen (Wi-Fi page) and in the panel (System > Device card) turns it off; off publishes an
   empty NDEF. Default on.
2. **USB host mode through the Type-C port controller.** `mu5358-usb host|device|auto` prefers the port's
   `port_type` (`source`, `sink`, `dual`: TCPM re-evaluates the attachment and VBUS; the fork's method, and the
   bench unit's kernel exposes it writable) over the data-role swap (needs a PD partner) that W5 ended with;
   the 60-second self-undo stays; `status` reports the policy and `musb_hostenable`. A "USB role" card on the
   panel's device page. The physical OTG test (adapter, stick, NIC) is the maintainer's.
3. **A host simulator and a fuzzer for u30ui2.** The dashboard compiled for the Mac/Linux host with a virtual
   clock and a scripted event language (tap, swipe, hold, key, wait, page, lang, set, uevent, shot); PNG
   screenshots; a Python test file that drives real scenarios (every page, navigation and settle, settings
   toggles, hold-to-confirm, lock, idle timeout, event wake, power menu) and a seeded fuzz run under ASan/UBSan.
   The device-side test hooks (`/tmp/u30ui.cmd` + SIGUSR2, SIGUSR1 → PNG) stay as they are.
4. **Event-driven wake on charger plug and unplug.** u30ui2 subscribes to the kernel's uevent multicast and
   turns the screen on when `ac` or `usb` of `/sys/class/power_supply` changes `online`; no new timer or
   poll. A lock stays locked; full, limited or thermally stopped charging does not wake the screen.
5. **The power key opens a menu instead of killing the device.** Today `buttons` powers off after a 3-second
   hold. New: a 1.5-second hold wakes the screen and opens u30ui2's Power page (restart, power off, modem,
   Wi-Fi, screen off; the first three confirmed by the existing 1-second hold ring). The blind power-off
   remains only when the dashboard is not the screen (classic UI or the console), still at 3 seconds.

Also, a small one found while reading: u30ui2 must draw its first frame before its first mode-set, so the
start of the UI never shows a black frame (the fork fixed a black gap at a different place, in its own panel
driver; our display driver is the vendor's, so only the userspace half applies here).

## Non-goals

- The SIM topology, the physical SIM mux, the modem daemons, the kernel: the next packages.
- The fork's panel plugin system, themes, wallpapers, Chinese-only toasts: u30ui2 keeps its own look.
- NFC beyond the tag's Wi-Fi record: no card emulation, no reading phones, no RF on/off (the tag's RF is
  passive), nothing in the modem's NV.
- DisplayLink output and host-mode device drivers beyond what the GKI kernel has.
- The screen's black gap between the bootloader logo and the dashboard (seconds of boot, before S11): not
  this package.

## Anti-goals (what would make this a failure even if it works)

- A Wi-Fi key on a command line, in a log, in a test fixture committed with a real value, or in a bench
  transcript. The key goes to the NFC tool on stdin only; the tool prints "key N characters" at most.
- A USB role change on the bench that cuts the Mac's link without the on-board self-undo and the reboot
  guard; the maintainer's rule "never cut the USB link in a test without an on-board self-undo".
- A screen that wakes on every battery tick (the whole point of the uevent is to not poll).
- A power key that turns the device off by accident; a power key that cannot turn it off at all (the menu
  must open with the screen off and the UI locked).
- Reading the device's Wi-Fi configuration or the tag's current record into a transcript. Bench checks of the
  NFC tool mask the SSID and never print the key.

## Constraints

- The project's standing rules: never slot a, never the NV partitions, never the GPT, never misc from Linux,
  Linux stays a one-shot trial, no SIM data without the maintainer, never read the VPN config, never
  `clk_summary`, secrets never in the repository or on a command line, the bench password never printed.
- Strings on the screen only through `tools/u30ui2/i18n.h` + `mkfont.sh` (EN, TR, ZH); panel strings through
  `openwrt/panel/po/{tr,zh_Hans}/mu5358.po`.
- New binaries are static aarch64, built in `mu5358-mainline-build` like u30ui2 (`tools/u30ui2/build.sh`), and
  shipped by `openwrt/customize.sh`; the release's asset count and `tools/ci-release.sh` stay in step.
- Tests: `cd tests && TMPDIR=<ssd>/work python3 -m unittest discover -s . -p 'test_*.py'` plus
  `node --test tests/web/*.mjs`; host C builds run in docker (`alpine` with gcc, or `mu5358-mainline-build`),
  never from a downloaded directory; python `-I` for anything that reads downloaded files.
- Settings persist in `/etc/mu5358/<name>` files, carried by the installer's and updater's `CARRY` list.
- Services are procd scripts in `/etc/init.d`, one-shot where the fork's are one-shot; rpcd objects are
  shell plugins with allow-listed arguments (`openwrt/usb/rpcd-mu5358usb` is the pattern), with an ACL file.
- Attribution: the NFC tool, the sync logic and the simulator's command language come from the fork (MIT);
  the file headers say so.

## Decided

### 1. NFC

- `tools/nfc/mu5358-nfc.c` is the fork's `u30pro_nfc.c` with the bus, address and names as they are
  (`show | clear | wifi <ssid> open|wpa2 (key on stdin) | encode <ssid> open|wpa2 [key]`); `encode` stays
  for the tests. Built static by `tools/nfc/build.sh`; shipped at `/opt/mu5358/bin/mu5358-nfc`.
- `/opt/mu5358/bin/mu5358-nfc-sync [sync|on|off|toggle|state|supported]` (sh): the one writer, serialised by
  `flock` on `/run/mu5358/nfc.lock`. State file `/etc/mu5358/nfc` (`off`; absent or anything else = on). The
  hotspot is `wireless.ap0` (this board's only AP section, the one `buttons` and `customize.sh` use);
  `disabled=1` or radio off → clear. Encryption map: `none`/empty → open; `psk2*`, `sae-mixed*` → wpa2;
  anything else (`psk`, `sae`, `owe`, `wpa*`, enterprise) → unsupported → clear. The key is read with
  `uci -q get wireless.ap0.key` into a shell variable and piped to the tool's stdin; the variable is unset
  after; stderr of the tool goes to a mode-0600 file under `/run/mu5358`, never to the log with the key
  (the tool never prints it). Result line in `/run/mu5358/nfc-result`: `open`, `wpa2`, `disabled`,
  `hotspot_off`, `unsupported`, `no_tag`, `error`.
- Triggers: `/etc/init.d/mu5358-nfc` (START=95, one-shot `sync`, `procd_add_reload_trigger wireless`: fires
  on a LuCI apply), a hotplug script `/etc/hotplug.d/iface/95-mu5358-nfc` on ifup/ifdown of the AP's
  interface, the switch itself, and an explicit `mu5358-nfc-sync sync` after `wifi reload` in the two
  writers that bypass `reload_config`: `buttons` (the Wi-Fi key) and the screen's hotspot toggle. A static
  test pins that every `wifi reload` in the shipped scripts is followed by the sync. The plan's first bench
  step records which netifd events an SSID-only change raises (the hook logs event names only, never the
  configuration); if a `wifi reload` keeps the netdev, the explicit calls are what carries the change, and
  that is acceptable. No polling. (Builder check, decided here.)
- Screen: a row on u30ui2's Wi-Fi page, `NFC tap-to-connect: On/Off/Unsupported`, tap toggles
  (`data_run("mu5358-nfc-sync toggle")`), a toast with the result; `data.c` reads the state file and the
  result file in its slow loop (same cadence as `usb-storage`).
- Panel: card `NFC` on System > Device (`openwrt/nfc/nfc.js`, loaded through `device.js` the way
  `usb-storage.js` is): state, what the tag holds (`hotspot <ssid>` / `empty` / reason), buttons On/Off; rpcd
  object `mu5358nfc` (`get`, `set {"value":"on|off"}`), ACL `acl-mu5358nfc.json`. The card never shows the key.
- The installer's `CARRY` gains `etc/mu5358/nfc` and `tests/test_static.py::test_carry` names it; the
  updater's `keep_list` already keeps `etc/mu5358` whole, nothing to add there.
- Boot order: S95 runs after the network; the first sync is asynchronous; a missing tool or bus = `no_tag`,
  logged once, never respawned.

### 2. USB host

- In `openwrt/usb/mu5358-usb`: `host|device|auto`. `auto` is new (`port_type = dual`). Order of attempts for
  `host`: `port_type` ← `source` when the attribute is writable; else the existing `data_role` swap.
  `device`: `port_type` ← `sink` (never a source: for a computer that is itself a source this is the
  role the port takes anyway); `auto` restores `dual`, the port's default policy, and is what the self-undo
  writes. The self-undo after `MU5358_HOST_UNDO` (60 s) restores `dual` unless
  `/run/mu5358/usb-host-keep` exists. `status` adds `policy: auto|host|device` and
  `host capable: yes|no` (`musb_hostenable` = `enabled`).
- rpcd `mu5358usb get` adds `policy` and `host_capable`; `set {"kind":"role","value":"auto|host|device"}`.
- Panel: card `USB role` on the device page (`openwrt/usb/usb-role.js`): the three buttons, the current
  policy, the data role, and the sentence that host mode undoes itself after 60 s unless kept.
- `host capable` is report-only (`musb_hostenable` under the musb platform device, found by a glob,
  `MU5358_MUSB` overrides it in tests); `host` is never refused on its account. (Builder check, decided.)
- Bench (mine): `status` first; then `auto`, `device` and `host` each as a guarded trial (the on-board
  reboot guard armed, the Mac's link expected to drop and return): whether `sink` or a re-written `dual`
  replugs the link is measured, not assumed, and the panel's card and the tool's messages say "may replug
  the USB link for a moment" for whichever does. The OTG test with an adapter is the maintainer's; the
  design does not claim host mode works until then.

### 3. The simulator and the fuzzer

- `tools/u30ui2/sim.c`: the host build (`build.sh sim`, gcc, `-DUI_HOST`) links `ui.c`, `i18n.c`, the shared
  gfx/icons/png/qr with stubs for display, input and data (as `preview.c` does) plus a virtual clock:
  `now_ms()` returns the simulated time, advanced only by `wait MS` and by the event timestamps.
  `ui.c` gets `#ifdef UI_HOST` seams only where the device loop reads the clock, the input and the signals.
- Command language on stdin, one per line: `tap X Y`, `down X Y`, `move X Y`, `up`, `swipe X1 Y1 X2 Y2 MS`,
  `hold X Y MS`, `key CODE short|long`, `wait MS`, `page NAME`, `lang en|tr|zh`,
  `set FIELD VALUE` (battery, charging, signal, usb_storage, nfc, update, lock...), `uevent SUPPLY 0|1`,
  `screen on|off`, `shot NAME` (PNG at `<dir>/NAME.png`), `state` (one line: page, screen, locked, toast).
  Unknown lines fail the run.
- `tests/test_u30ui2.py`: builds the simulator in docker once per run, then scenarios: every page in every
  language renders (and the PNGs are not blank), swipes move pages and settle, swipe down goes home, the
  settings rows cycle, the Power page's 1-second hold runs the action (the stub records the command), the
  lock holds after the idle timeout, the idle timeout turns the screen off, a `uevent ac 1` turns it on, the
  power-menu command opens the Power page with the screen off, the NFC row toggles. Then `fuzz`: 4000 random
  events from a seed against the ASan/UBSan build; the seed is fixed in the test and a second seed is
  `MU5358_FUZZ_SEED`.
- The PNGs of a run go under `TMPDIR`; `tools/u30ui2/preview` (sample pages) stays.

### 4. Event wake

- Which supply announces a plug on this kernel is not known from the repository (the charger manager
  floods `battery` events; `usb`, `ac` and the TCPM source supply may or may not call
  `power_supply_changed`). The plan's first step is a bench probe: a temporary `hotplug.d/power_supply`
  script that logs `ACTION`, `POWER_SUPPLY_NAME` and `POWER_SUPPLY_ONLINE` only, one plug and one unplug
  of the Mac's cable. The supplies that carry the edge become the filter (`ac`, `usb`, or the TCPM source
  supply counts as the charger); `battery` never does. (Builder check, decided.)
- `ui.c` (device build): `NETLINK_KOBJECT_UEVENT` socket, group 1, non-blocking, in the main `poll` set;
  a packet with `ACTION=change`, `SUBSYSTEM=power_supply`, the chosen `POWER_SUPPLY_NAME` and
  `POWER_SUPPLY_ONLINE=0|1` different from the last seen value → `wake()` (backlight on, `last_touch = now`,
  redraw); no unlock. Baseline read once from sysfs after the subscription. `ENOBUFS` → one re-read, no
  timer. A failed socket is logged once and the UI runs without it.
- The simulator's `uevent` command feeds the same parser.

### 5. The power key

- `buttons` (in `customize.sh`) reads the keys with `mu5358-keys -l 1500` when the dashboard is the screen
  (`/etc/mu5358/ui` not `classic` and `/opt/mu5358/bin/u30ui2` present), with `-l 3000` otherwise; the
  threshold is picked at start, and `mu5358-screen` restarts `mu5358-buttons` whenever it starts, so a
  switch of the UI re-picks it. `116 long` with the dashboard: write `power` to `/tmp/u30ui.cmd` and send
  u30ui2 SIGUSR2; when no u30ui2 process exists at that moment (the classic UI, the console, or the
  dashboard crashed and procd has not respawned it yet): `poweroff`, as today. Decided this way because a
  key that cannot turn the device off is the worse failure (no other way in without a computer); the
  accidental-off window is the respawn gap only, and the long press is still 1.5 s. (Builder check,
  decided.)
- u30ui2's command `power`: screen on, page = Power, `last_touch = now`. The lock does not block it: a held
  physical key is the owner's act, and the Power page's own 1-second hold ring still guards the actions.
  Swipes and taps elsewhere stay locked.
- `116 short` unchanged (backlight toggle by `buttons`).

### 6. First frame

- `display_init()` zeroes the buffers and sets the mode; `main()` then draws, and the panel's MADCTL turn
  (`panel_turn`, after the mode-set because it shares the SPI bus with the frames) decides the copy
  direction of every frame. Decided: `display_init` takes a callback that draws the first frame into the
  buffer before `drmModeSetCrtc`, with `panel_turn` moved before the mode-set when the bench shows the bus
  idle there; if the first frame then shows flipped for one frame, the status quo (one black frame) is
  kept and the item is closed as measured. Low stakes, bounded to one bench try. (Builder check, decided.)

## Left to the builder

- File names under `openwrt/nfc/` and `tools/nfc/`; the exact rpcd field names beyond those listed.
- The hotplug script's event filter (which `ACTION`s on which `DEVICENAME`s re-sync).
- How `sim.c` stubs `data_run` (record commands to a list the `state` command prints).
- The PNG blank check (a pixel count over a threshold).
- Which docker image compiles the host tools (alpine gcc with ASan, or `mu5358-mainline-build`).

## Verification I will do, and what stays with the maintainer

Mine: the test suite (python and node) green; the release built by CI; on the bench: `mu5358-nfc show` with
the SSID masked (never the key), a sync after a hotspot toggle, the screen and panel switches, `mu5358-usb
status` / `auto`, one `host` trial with the guard, the UI's uevent wake by plugging the Mac's cable, the power
key menu, a boot with the new image. Maintainer: a phone touching the device joins the hotspot; the OTG
adapter test; the look of the new rows and the card.
