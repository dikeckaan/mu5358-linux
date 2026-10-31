# Parity with mu300-linux: the gap list

Audit of 2026-10-30, read-only, of mu300-linux's whole history (1728 commits, 2026-09-16 to 10-30, 65 tags) against
this repository. `M/` = mu300-linux, `BIN` = `M/rootfs/overlay/opt/mu300/bin`, `OW` = `M/openwrt/overlay`,
`§N` = `M/docs/FINDINGS.md` section N. AS-IS = works once enabled; ADAPT = board work; N/A = does not apply.
Effort S (< half a day), M (a day or two), L (more).

**The fact that decides most of this list:** `openwrt/build-rootfs.sh` and `tools/make-release.sh` take mu300's
*plain* `mu300-openwrt-rootfs.tar.gz`, not `mu300-openwrt-luci-rootfs.tar.gz` (`M/openwrt/build-rootfs.sh`
`MU300_SYSTEM=openwrt-luci`, l.41-116). The plain image has stock LuCI only: no `luci-app-mu300`, no `mu300-smsd`
SMS pool, no `mu300-atd-dash`, no ttyd, no tr/zh-cn catalogs of the panel, no `mu300cell-v6.sh`. Changing the base
tarball gets all of it (B1). `openwrt/customize.sh` disables every `mu300-*` service but `mu300-accounts`, turns
`mu300-atd` back on; every `BIN` tool is still in the image and on the PATH.

## Do first (top 15)

Status on 2026-10-30 evening, after the user's decisions (a password always; the panel and every mu300 tool
adopted under the mu5358 name; no telnet; the rest to be applied):

1. C1 root password - **done**: the page hashes it (sha512-crypt in the browser), `mu5358-install.sh password`
   takes the hash on stdin and writes the system's `/etc/shadow`; an install refuses to run without one
2. B1 base on the `openwrt-luci` tarball - **done**, and more: `openwrt/rebrand.sh` renames the whole tree to
   mu5358 (tools, services, uci, ubus `mu5358dash`, `luci-app-mu5358`, catalogs rebuilt from `openwrt/panel/po`);
   the panel verified in Chromium on the board (dashboard, cellular, SIM, SMS pool, traffic, Wi-Fi, battery)
3. A1 `mu5358-device` answers `mu5358` - **done** (rebrand.sh)
4. C2 telnet - **done**: removed from `boot/init` (the ACM console stays; SSH is the way in)
5. A2 crash evidence - **partly**: lockup detectors + hung_task panic (`90-mu5358.conf`); logd 1 MiB comes with
   the base; a collect-logs script for Android is still to write
6. A3 `thermal-guard` - **done** (`mu5358-thermal`), with a fix this board needed: only the zones that are sensors
   (`frame-thmzone` read 86 C on a cool board and the guard throttled the CPUs)
7. A4 `mu5358-traffic`, `mu5358-toolkit` - **done** (linked; `mu5358-cpu`, `-modem-log`, `-ndp` still off, to verify)
8. B2 SMS pool - **comes with B1** (`mu5358-smsd`, `mu5358-sms list` works); send and the unread badge on the
   dashboard are still to do
9. D1 signed releases - **done**: `tools/release-keygen.py` (key 15fd2f702186c19e; the secret stays with the
   maintainer), `tools/sign-release.sh` in `make-release.sh`, `tools/usign-verify.py`; the page verifies
   `SHA256SUMS.sig` (WebCrypto Ed25519, a JS fallback) and refuses an unsigned release; `release.pub` in every image
10. B3 a safe updater - **done, from Android**: `mu5358-install.sh update` writes the new system over the installed
    one with the settings carried (`CARRY`: uci config, passwords and keys, the SMS pool and traffic ledger, the
    screen's settings, the web server's own key), then `boot_b`/`init_boot_b`; misc never; a downgrade refused.
    Verified 2026-10-30 (v2026.10.30.5 → .6 with the settings intact). From Linux: still to do
11. A5 `mu5358-power` - to do (service present, not linked)
12. D2 tests + CI - **done**: `tests/` (46 Python tests: the hard rules as static checks, the installer's helpers
    under busybox, the web installer under node; 31 node tests) and `.github/workflows/tests.yml`
12b. D4 the release workflow - **done 2026-10-31**: `.github/workflows/release.yml` + `tools/ci-release.sh` on
    `ubuntu-24.04-arm`: the kernel from the ACK commit (shallow, `.git` removed, the release string and every
    exported symbol's CRC checked against `kernel/Module.symvers`), the tools in the same images as on the Mac,
    busybox from Ubuntu's `busybox-static` (byte-identical to mu300's), the rootfs by its digest, the signature
    with the secret `MU5358_RELEASE_KEY`, the GitHub release (prerelease by default, assets' digests checked
    back), then the mirror. `gltest` is a prebuilt (`gpu/prebuilt`: it links against the device's Mali library);
    the two Android tools are musl static builds there instead of the NDK's
12a. D5 the release mirror - **done 2026-10-31**: the page downloads a release's files itself (`mirror.js`,
    `config.js`), from the branch `mirror/<tag>` of this repository on raw.githubusercontent.com (parts of at
    most 45 MiB, `tools/make-release-mirror.sh`, pushed by `tools/publish-mirror.sh`; no second repository and no
    deploy key, unlike mu300), every part and file checked against GitHub's digest and then the signed SHA256SUMS
    as a picked file would be; picked files stay the fallback. The device's own download (mu300's `run fetch`)
    is still to do: it costs the SIM's data
13. E1 screen UI strings en/tr/zh - **done** (`tools/u30ui2`, the dashboard)
14. A6 Wi-Fi self-healing - **partly**: `mu5358-wifiwatch` (`openwrt/wifiwatch`: hostapd/wlan0 watched, `wifi up`
    after three misses, once in 10 min); the firmware-reset signature of the AIC8800 and regdb still to look at
15. D3 release smoke test - to do

## A. Stability and robustness

| # | Item | mu300 | Status | Effort | Deps |
|---|---|---|---|---|---|
| A1 | **Board identity.** `mu300-device` answers `f50` for any unknown board; `mu300-api`, `dashboard-info`, `mu300-usb`, `mu300-power`, `mobile-data`, `mu300-lan-ip` branch on it. Add `mu5358` (from `/etc/mu5358-device`) and audit each `case $(mu300-device)`. Keep the `--json` fields (`M/docs/ADB-INTERFACE.md` calls them a contract). | `BIN/mu300-device`, `§95` | ADAPT | S | - |
| A2 | **Crash evidence.** mu300: `log_buf_len=4M`, soft/hard lockup detectors with panic (`§31d`), logd 1 MiB (`OW/etc/uci-defaults/90-mu300` l.143, `§191`), `early-recorder` (`§3`), `console-ramoops` reading (`§20b`), `M/tools/collect-logs.sh`. Here `mu5358-log` snapshots to the persistent log (good), `tools/sysdumpdb-extract.py`. Missing: lockup detectors + `hung_task_panic` (an RTNL-held oops hung every network command on 10-30; a lockup would do the same without a reset), logd size, one `collect-logs.sh` for Android (persistent log + sysdumpdb + uboot_log), `bootmark` timeline lines (AS-IS). | `§3 §20b §31d §191` | ADAPT | S | - |
| A3 | **Thermal.** `thermal-guard`: watches when the kernel throttles, caps above BACKSTOP, powers off at CRIT, battery temperature too. mu5358 loads `sprd_cpu_cooling`/`sprd_virt_thm` but nothing checks a passive trip is bound (`§139`, issue #229: a unit overheated and reset). Drop the `mu300-led alarm` calls (u30ui instead); run as a procd instance (respawn 3600 5 0). | `BIN/thermal-guard`, `§48 §139 §166` | ADAPT | S | A1 |
| A4 | **Boot-time applies** lost to the blanket `rm` of `mu300-*`: `mu300-traffic` (S96: counters to RAM per minute, disk per 30 min and at stop; `ntp` hotplug dates it), `mu300-toolkit apply` (S96: TTL, TCP, profile), `mu300-cpu` (S95: cores, lowlatency `§200`), `mu300-modem-log` (S92 `modem-log-drain`: the F50's CP stalls at ~90 s without it; the soak here was clean, so verify `spool` in `/proc/devices` first). Re-enable selectively in `customize.sh`. | `OW/etc/init.d/mu300-{traffic,toolkit,cpu,modem-log}` | AS-IS / verify | S | - |
| A5 | **Power.** `mu300-power` (1092 lines, `§36`): plugged/battery/saver, idle timers for Wi-Fi, radio (`mobile-data suspend`, `AT+SPENDC=2`, `AT+SFUN=5`) and CPU, charge limit (U30 Air bq256xx), charging-boot marker. README lists suspend, unplugged operation and charging under 100 % as untested. Port without the LED paths; check the charge guard against this charger-manager. `§37` suspend is separate (L). `mu300-modem-coldboot` (`§196`) is U30 Air binary patching: N/A unless a charger boot here shows the same. | `BIN/mu300-power`, `§36 §37 §196`, `M/docs/superpowers/specs/2026-10-06-power-profiles-design.md` | ADAPT | M + L | A1 A3 |
| A6 | **Wi-Fi self-healing.** mu300: `wifiretry` (45 s, `ubus call hostapd.wlan0 get_status` else `wifi up`), `misc/10-mu300-wcn` reopen on `FW_ERROR`, `openwrt-board-check` (corrupt board.json, generic), `regdb-load` (`§22`) before `iw reg set`, `94-mu300-ap-uapsd` (`§203`), no-DFS patch (`§42`, in the tarball). Here `mu5358-hw` does one `wifi up` and never looks again; `regdb-load` does not run (check `iw reg get` shows TR with the vendor cfg80211's certificates); the AIC8800 firmware-reset path is unknown: find its dmesg signature, add a hotplug like `10-mu300-wcn`. | `OW/etc/init.d/mu300-post`, `OW/etc/hotplug.d/misc/10-mu300-wcn`, `BIN/regdb-load` | ADAPT | M | - |
| A7 | **Mobile data watchdog, all of it.** `mobile-data watch` runs (`mu5358-cell`), but its downlink-stall ladder (`§176`: VPN restart, `ifup wan`, reboot at most 3, `network.wan.dl_reboot`), `§90`, `§13g` (idle SIPA reads as broken), roaming hold `§120`, SIM-absent hold (`§186`, `+ECIND` from atd) assume `sipa_eth0`, `mu300-device` and `mu300-led`. Verify each branch; a ladder reboot lands in Android here (fine, but log it to the persistent log first). | `BIN/mobile-data` | verify | S | A1 |
| A8 | **AT daemon.** On already (`§13b §138`). Check `§40` (a stuck command wedged the panel; per-command second) and `§26d` (USSD reads nr0's URC log) with this firmware. | `BIN/mu300-atd`, `BIN/mu300-at` | AS-IS | S | - |
| A9 | **USB.** mu300: early DHCP across switch_root (`06_mu300_early_usb`, active here), `mu300-usb-reset --if-no-lease`, `§12 §38b §205 §213 §237`. Here `usb-watch` (carrier + TX-stall, growing gaps) is already better for this MUSB; `10-mu300-usb` is patched. Gap: NCM/ECM/RNDIS choice (`mu300-usb net`, `/etc/mu300/usb-net`) for Windows (`§38b`); `boot/init` hard-codes NCM. | `BIN/mu300-usb`, `§38b` | ADAPT | S | - |
| A10 | **adb to Linux** (`§43 §70 §82 §213`): adbd as FunctionFS in the same gadget, `/etc/mu300/adb`, `adb_auth open|keys`. Off here. Needed for B7 and `smoke.py`; FunctionFS next to NCM on this MUSB + GKI needs its own test. | `BIN/mu300-adbd`, `M/tools/adbd` | ADAPT | M | A9 |
| A11 | **Module autoload guard** (`§163`, `98-mu300-modules.conf` in the image; `mu300-modprobe-guard --prepare` ran from `mu300-hw`). GKI modules sit in `/root/gki-modules-disabled`, so nothing autoloads yet; enable when they move to `/lib/modules`. | `BIN/mu300-modprobe-guard` | later | S | - |
| A12 | **boot/init.** mu300: `/run` 0755 nosuid nodev (PR #453, d28c2006), `§33a` wait for the eMMC, `§38a`, `§160` HS400ES retry. Here `panic_on_oops` is 0 in init and 1 from sysctl later: make it 1 from the ramdisk. The 5-minute safety reboot (l.94) is a trial-era guard (`§33h`): keep, document. | `M/boot/init` | ADAPT | S | - |
| A13 | `31_mu300_failsafe_wait`, neutralised `rc.button/*`, `zram-start`/`mu300-zram` (`§27`): active and fine. | - | AS-IS | - | - |

## B. Features and polish

| # | Item | mu300 | Status | Effort | Deps |
|---|---|---|---|---|---|
| B1 | **The control panel.** `luci-app-mu300` (views home, sim, simcard, locks, sms, forward, at, settings, ttl, traffic, device, update, backup, languages, power, leds, cpu, terminal, packages, vpn-*; rpcd `mu300dash` 1417 lines, adapters `root/usr/libexec/unisoc-modem/`, ACL, syslog/channel-analysis overrides) plus `luci-overlay` (`91-mu300-luci` Aurora + IPv6 relay, ttyd on `@lan`, `mu300-atd-dash` nr6/nr7, `mu300-smsd`/`mu300-sms`, `mu300-ndp`, `mu300cell-v6.sh`). Change `tools/make-release.sh` ROOTFS to the `openwrt-luci` asset and its SHA256. Then hide (drop the `menu.d` entry) or adapt: `leds.js`, `sim.js` (f50/u30air/v50 gate), `cpu.js` voltage (`§201`, never here), `power.js` battery tile, `device.js` USB role, SC2355 station parsing in `dashboard-info`, `channel_analysis.js`. | `M/openwrt/luci-app-mu300`, `M/openwrt/luci-overlay` | ADAPT | M | A1 C1 |
| B2 | **SMS.** `sms` (PDU `§26c`), `mu300-cmt-spool` (`+CMT` acked once on flash `§51`), pool `mu300-smsd` (`§39 §63`), `mu300-sms-forward` (webhook/Telegram/e-mail/SMS, `§54 §58 §137`), `§46` SMSC, `§49` own number. u30ui reads the SIM with `sms` every 30 s and cannot send; with B1 it should read `/etc/mu300/sms-pool` (unread state for the badge) and send with `sms send`. | `BIN/sms`, `BIN/mu300-cmt-spool`, `BIN/mu300-sms-forward` | AS-IS / ADAPT (u30ui) | M | B1 |
| B3 | **Updates, safely.** `mu300-update` is a stub here, rightly: `unlock_for_trial()` (l.293-320) calls `mu300-next-boot --trial`, `boot_update_run` (l.3437-3597) writes `boot_$(linux_slot)` (= `boot_a` when Android is on b), `--rearm` is mu300-post's bootok. Reusable: self-replace with the release's updater, download-and-verify-all-first, `<os>.new` unpack + `mu300-carry` of the user's files (`§85 §131`, `image-manifest.py`, `make-diffs.sh`), `mv` swap with `<os>.old` rollback, `write_boot` (l.3068: dd, drop_caches, sha256 read-back, prev.* kept), flock in a 700 `/run` dir (`§81`), `notice` every 6 h, `sig_state`/`sums_verdict` (l.4082-4192). **Proposal:** (a) from Android, cheapest and mostly written: `mu5358-install.sh update` = `take` + the `system` and `boot_images` steps with `/etc` carried by manifest, backup kept; misc untouched; `boot-linux` tries it. (b) from Linux: the region is one ext4, not mu300's disk of systems, so either an A/B pair of trees in it with the swap at the next boot, or a `.new` tree and `switch_root` from `boot/init`; write `boot_b`/`init_boot_b` only (refuse any other slot, refuse when Android's slot is `_b`), read back, never misc. | `BIN/mu300-update`, `M/tools/carry/mu300-carry.c`, `§81 §85 §126 §131` | ADAPT | L | D1 B4 |
| B4 | **Settings backup** `mu300-backup export|import` (`§155`, AES-256-CBC, 0600), `mu300-api backup.*`, `backup.js`. | `BIN/mu300-backup` | AS-IS | S | A1 |
| B5 | **USSD, TTL, TCP.** `mu300-ussd` (`§26d`); `mu300-ttl` (tc pedit, else nft `inet mu300_ttl`: GKI 6.6 has nft via `kernel/patches/0001-nftables-kmi.patch`, check `act_pedit`/`cls_matchall`); `mu300-tcp` (check `tcp_bbr`, `sch_fq` in the GKI set). | `BIN/mu300-{ussd,ttl,tcp}` | verify | S | A4 |
| B6 | **Wi-Fi band and client mode.** `mu300-wifi-band` (channel lists in `lib/wifi-channels.sh` hard-code SC2355's 36-48/149-165 `§23 §42`), `wifi-client` (1002 lines, `§14b`: one-way at boot on SC2355). AIC8800 may do AP+STA: measure, then adapt `wifi-client` or write a small netifd one. | `BIN/mu300-wifi-band`, `BIN/wifi-client` | ADAPT | M | A6 |
| B7 | **Device manager** over adb to Linux: `mu300-api` (`§110`, 2018 lines, JSON on stdin, every field validated), `device.js` LinuxDevice. | `BIN/mu300-api`, `M/docs/ADB-INTERFACE.md` | ADAPT | M | A1 A10 D1 |
| B8 | **VPN.** External module (`mu300-extra install vpn`: xray/sing-box/mihomo/dae, `§26b §26e`), kill switch fail-closed (`lib/vpn-orphan.sh`), `mu300-vpn-uplink` + hotplug `96-mu300-vpn-uplink` (`§176`), `§170`. Extras live at `/mnt/mu300-disk/extra`: no such mount here, so `mu300-extra` needs a disk path in the region first. LuCI VPN pages come with B1; `wireguard-tools` + `luci-proto-wireguard` are in the image. | `BIN/mu300-extra`, `BIN/mu300-vpn-uplink` | ADAPT | M | B1 |
| B9 | **Toolkit TUI** `mu300-toolkit` (menu, top, stress, info; update notice at login), **languages extra** (`mu300-extra lang`, 31 languages), **user packages after an update** (`mu300-user-packages`, `§85`). | `BIN/mu300-toolkit`, `BIN/mu300-user-packages` | AS-IS | S | B3 B8 |
| B10 | **u30ui vs mu300's LEDs.** u30ui already shows what `§34 §151 §152 §154` lit. Missing on it: thermal alarm (A3), update notice (B3), SMS send and unread (B2), charge limit (A5), adb state (A10), APN/roaming/VoLTE settings (`network.wan.*`, `§65 §120 §184`, in `mobile-data` already; `/etc/config/network` 0600). | `tools/u30ui` | ADAPT | M | A3 A5 B2 B3 |
| B11 | **Buttons** `mu300-buttons` + `mu300-keys` (b3f199a8 stop semantics): `buttons` here is a clean rewrite. | `BIN/mu300-buttons` | done | - | - |

## C. Security

| # | Item | mu300 | Status | Effort | Deps |
|---|---|---|---|---|---|
| C1 | **Root password.** mu300's image root has none (`§47`, #167); every installer sets one (`M/install.sh` l.1147 `openssl passwd -6`, `M/tools/android-install.sh` l.780-990, `M/tools/sha512crypt.py`), keeps it across updates (`§126`: `M/tools/android-pwcheck.sh` + static `M/tools/pwcheck/mu300-pwcheck.c`, the hash checked as data, nothing of Linux executed), `M/tools/reset-password.sh`. Here nothing sets one (`grep -i passwd installer/mu5358-install.sh openwrt/customize.sh boot/init`: no hit): dropbear and LuCI take an empty root password from any LAN client, and the Wi-Fi password is on a QR code. Add a password step to `mu5358-install.sh` (`$6$` hash from the page, or Magisk busybox `mkpasswd`), `reset-password`, keep-password on update. | `§47 §126` | ADAPT | M | - |
| C2 | **telnet in the ramdisk.** `boot/init` l.235: `telnetd -l /mu/bin/sh` on 192.168.58.1 at every boot. mu300 only on its rescue path (`M/boot/init` l.1067, `-b $NET.1`, rootfs unavailable). Bind it and start it only in the no-root/fsck-failed branches; the ACM shell is enough otherwise. | `M/boot/init` l.1067 | ADAPT | S | - |
| C3 | **Release audit.** mu300's `make-release.sh` refuses an image with host keys (`etc/dropbear/*host_key`), `uhttpd.key`, machine-id, hotspot/vpn/mobile-data conf, firmware, vendor files, and checks the manifest (`§132` is handled here: key deleted, `92-mu300-uhttpd-key` runs). Add the same audit to `tools/make-release.sh`. | `M/tools/make-release.sh` "audit failed" | ADAPT | S | - |
| C4 | **Bound to the LAN.** mu300: dropbear/uhttpd stock behind fw4's wan zone (`90-mu300` names devices, `§38c`), ttyd on `@lan`. fw4 was off here until the nft patch: confirm `fw4 print` has the wan zone on `sipa_eth0` (`fw4-sipa-offload.patch` is in the tarball) and `earlyusb` holds. | `OW/etc/uci-defaults/90-mu300` | verify | S | - |
| C5 | **/run** (`§60`, PR #453): `lib/run-dir.sh` guards `mu300-atd`/`mu300-api`; mount `/run` 0755 nosuid nodev in `boot/init` as `M/boot/init` now does. | d28c2006 | ADAPT | S | - |
| C6 | **Secrets never on argv** (`§65`, 85630d60): `mobile-data`, `sms`, `mu300-at -` use env/stdin. u30ui must do the same when it sends. | `BIN/sms` | AS-IS | - | B2 |
| C7 | **adb policy** (`§70`): mu300 ships on + open; when A10 lands, default `keys`. | `§70` | decision | - | A10 |
| C8 | **Supply chain.** mu300 verifies kernel.org with pinned OpenPGP (`§161`; `mainline/verify-kernel-src.sh` exists here) and OpenWrt's sums with `M/tools/usign-verify.py`. The base rootfs is pinned by SHA256 in `make-release.sh`; verify its `SHA256SUMS.sig` with `release.pub` too. | `M/tools/usign-verify.py` | ADAPT | S | D1 |

## D. Installer, site, release, CI, tests

| # | Item | mu300 | Status | Effort | Deps |
|---|---|---|---|---|---|
| D1 | **Signed releases.** `sign-release.sh` (usign Ed25519 via OpenSSL `pkeyutl`, key in `MU300_RELEASE_KEY`, verified against `release.pub` before publishing), `mu300-release` binds the tag, `SHA256SUMS.sig`, `§125` tags `vYYYY.MM.DD.N` (one `tag_ok` block in every tool, held to `tests/fixtures/release-tags.tsv`). Here `SHA256SUMS` only; the page trusts GitHub's API. Add a key, the `.sig`, Ed25519 checking in the page (JS) and on the device (`usign -V`). | `M/tools/sign-release.sh`, `M/tools/release_pick.py` | ADAPT | M | - |
| D2 | **Tests and CI.** ~105 `tests/test_*.py` (stdlib): `helpers.py` (`ShellTest`, dash/bash/busybox), `fakeadb.py`, `fakeemmc.py`/`fakegpt.py` (whole eMMC file with GPT, misc, boot slots), `test_static` (~70 rules from past bugs), `test_boot_init`, `test_boot_image`, `test_magisk_switch` (misc bytes), `test_run_dir`, `test_case_sensitive`, `test_web_installer` + `tests/web/*.mjs` (fake adbd), `shard.py`; `tests.yml` (4 shards, busybox-only on PRs), `installer.yml` (`sh -n`, i18n check). Here none. Start: static rules for the hard rules (no write to `*_a`, `nr_*nv*`, `prodnv`, `misc` outside `boot-linux`/`uninstall`), `fakeemmc` for `mu5358-install.sh status|take|install`, `boot/init` misc-restore test, `tests.yml`. | `M/tests/`, `M/.github/workflows/tests.yml` | ADAPT | L | - |
| D3 | **Smoke test** `M/tools/smoke.py` (`§156`, `docs/RELEASING.md`): SSH host key pinned, snapshot, update, reboot, identity again, services/hotspot/mobile-data, strict patterns. Here the 3 h soak is a one-off loop. | `M/tools/smoke.py` | ADAPT | M | - |
| D4 | **Release workflow.** `release.yml` (dispatch, main only, refuses an existing tag, `ubuntu-24.04-arm`, `make-release.sh TAG --publish`), manifests, diffs, mirror (`make-release-mirror.sh`: 45 MiB parts, page trusts GitHub's size+SHA), `pages.yml`, `telegram.yml` + `telegram-announce.sh` (wiki "New in" bullets, token via `curl -K`). Here `make-release.sh` runs on the Mac with docker + NDK. Add `release.yml` (`kernel/Dockerfile` is Linux-ready), `pages.yml`, `telegram.yml`, `--publish`. | `M/.github/workflows/`, `M/tools/telegram-announce.sh` | ADAPT | M | D1 |
| D5 | **Web installer.** mu300's (`M/site/assets/web-installer/`, `docs/web-installer.md`): WebUSB adb with RSA key store, `github.js` digest pinning, `mirror.js`, `flow.js` answers, `privacy.js` masking, `demo.js`, `strings.js` en/tr/zh held equal by `test_web_installer.py`, `manager.js`. Here one 16 KB page in en/tr. Gaps: zh, mirror, masking, demo, LinuxDevice (A10/B7), tests, "download the backup". | `M/site/assets/web-installer/*`, `M/tests/web/` | ADAPT | L | D1 E2 |
| D6 | **Installer guards** (`§30 §50 §57`): read-back after cache drop (here `write_part` reads back: keep), bounded stages with a stall detector, a per-`boot_id` stalled marker, unknown GPT reported not computed. Forbidden patterns to keep out: `§112` recovery BCB in misc, `android-bootchain.sh sync` of uboot/sml/trustos/vbmeta, slot a. The `CHAIN` copy here is slot-b only and read back: keep. | `M/install.sh` l.390-411, `§50` | verify | S | - |
| D7 | **Site and docs.** mu300: index, choose, features, install, troubleshooting, updates, web-installer in en/tr/zh, `make-diagrams.py`, hreflang sitemap, `M/wiki/` (22 pages), numbered `FINDINGS.md`, `BUILD.md`, `RELEASING.md`, `ADB-INTERFACE.md`. Here index + web-installer (en/tr), `GKI-SYSTEM.md`. Add troubleshooting (persistent log from Android, sysdumpdb, "power off on USB restarts"), updates, zh, a numbered `docs/FINDINGS.md` (RTNL oops, clk_summary hang, MADCTL tearing, NV-in-RAM, USB stall). README's "no installer and no release yet" is stale. | `M/site/`, `M/wiki/`, `M/docs/` | ADAPT | M | E2 |

## E. i18n

mu300 translates four things, none machine-driven at build time:

1. **Installers**: `M/i18n/{tr,zh}.tsv` (~898 lines, `English<TAB>text`, `{1}..{6}`), `M/tools/i18n.sh` (`t()` awk lookup, `choose_language`, `normalize_answer` evet/hayır/是/否), `M/tools/check-i18n.py` in `installer.yml`. Here the device script is English; the page translates. Decision: keep `mu5358-install.sh` English (its `MU5358-*` lines are machine lines) and carry the human text in the page (S).
2. **LuCI panel**: `_()` in JS, `# i18n:` in the backend, `po/<lang>/mu300.po` (31 languages; tr and zh_Hans required, 1436 msgids), `M/tools/luci-i18n.py check|extract|update` (cjk, missing, placeholder, stale, dynamic, no `N_`), `M/tools/po2lmo.py` (byte-exact port), `luci-languages.tsv`, compiled in `build-rootfs.sh` l.107-116, `test_luci_i18n.py`/`test_po2lmo.py`; `luci-lexical-translator.sh` (#376). **Comes with B1**; a page we change must be in tr and zh_Hans (reuse `luci-i18n.py`).
3. **Cockpit** (`M/tools/cockpit-i18n.py`): Ubuntu only, **N/A**.
4. **Site**: hand-written parallel trees with hreflang, `make-diagrams.py`; web installer `strings.js` en/tr/zh held equal by a test. Here en/tr.

| # | Item | Status | Effort | Deps |
|---|---|---|---|---|
| E1 | **u30ui**: 28 Turkish literals in `tools/u30ui/ui.c`, one in `data.c`, no switch. A `strings.h` table (en/tr/zh) chosen by `/etc/u30ui.conf` LANG (default `uci luci.main.lang`), a Python key-equality test. CJK needs a font: a Noto Sans SC (OFL) subset of the glyphs the table uses, built like `font_*.h`, keeping the binary under ~2.5 MB. | ADAPT | M | - |
| E2 | **Web installer and site** en/tr/zh: `site/zh/`, one `strings.js`-style table instead of two page copies, a key-equality test. | ADAPT | M | D2 |
| E3 | **LuCI** tr/zh_Hans: with B1; changed pages through `luci-i18n.py update`. | AS-IS | S | B1 |

## F. Not applicable

| Item | Where | Why |
|---|---|---|
| Default Linux boot: `mu300-next-boot linux|--rearm|--trial`, bootok, `/etc/mu300/default-boot`, `§17 §100 §33h` | `BIN/mu300-next-boot`, mu300-post | One-shot trial by rule; `boot/init` restores misc at 1.6 s. The stub is right. |
| Linux on slot a when Android is on b; `android-bootchain.sh sync` of uboot/sml/trustos/teecfg/vbmeta (`§40`) | `M/install.sh slot_setup` | Slot a never written; the installer refuses unless Android is `_a`. |
| `§112` make-room by recovery BCB + resume (`resume.py`, `resize-last-partition.py`) | `M/tools/resume.*` | Writes misc and a wipe. `gpt-userdata` + `f2fs-shrink` shrink live, no misc. |
| Magisk zip / module switch (`§32b §101 §111`) | `M/android/magisk/` | `boot-linux` writes the block once from Android. A Magisk "start Linux" button could reuse `switch.sh`'s validated-hex `dd` (optional, S). |
| NV written by `cp_diskserver`/`modem_control` | `BIN/android-vendor-start` | RAM copies only (`modem/modem-start`). |
| LEDs: `mu300-led`, `04_mu300_early_leds`, `§34 §151 §152 §154 §199 §227` | `BIN/mu300-led` | No LEDs; u30ui. |
| SC2355: `wifi-start`, `wcn-reopen`, `10-mu300-wcn`, `hotspot-verify`, Bluetooth `§25`, `§14 §14c §21 §23 §31a §31i §45 §80 §92 §103 §106 §175 §180 §195 §203` | `BIN/wifi-start` etc. | AIC8800 on SDIO; generic parts in A6/B6. Bluetooth here is new work. |
| Mainline findings `§13d §13e §31*` (not 31d/31h), `mu300_thermal` `§48`, `§93 §102 §166 §206 §218`, `mainline.yml` | `M/upstream/` | Vendor GKI here; `mainline/` paused. |
| U30 Air / V50 / F50 hardware: SIM2 (`§74 §91 §104`), V50 cards `§204`, NFC `§33g`, USB-PD `§150`, fuel gauge `§105 §181 §197`, CPU voltage `§201`, `docs/SIM-SELECTION.md`, `docs/U30AIR-*.md` | various | Other boards. |
| Ubuntu/systemd: Cockpit, `kmsg-forward`, `hotspot-start`, `lan-start`, `rootfs-fixups`, `§11 §13 §19 §29` | `M/rootfs/panel` | OpenWrt only here. |
| SD card (`§9b §31j §31k §31l`) | `M/boot/init` | No slot. |
| `fw4-old-kernel-flowtable.patch`, `19-mu300-fw4` (`§38c`), procd-ujail removal | `OW/etc/hotplug.d/iface/19-mu300-fw4` | 5.4 only; self-limited, harmless. |
| `early-recorder` into `boot_b` at 48 MiB | `BIN/early-recorder` | Would write inside AVB-signed boot_b every boot; the persistent-log sectors replace it. |

**Order:** C1 and A1 first (everything branches on them); B1 makes B2/B4/B8/B9/E3 cheap; D1 gates B3/B7/D4/D5;
A10 gates B7 and `smoke.py`'s Linux side; D2 before B3 so the updater is born with a fake-eMMC test.
