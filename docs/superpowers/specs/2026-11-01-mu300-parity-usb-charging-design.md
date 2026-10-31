# Design: everything mu300 has, plus USB modes and real charging (2026-11-01)

What the maintainer asked for on 2026-11-01, in their words and order: the update mechanism, the system's
stability and "everything" as good as mu300-linux, with every improvement that reached mu300 checked and applied
here; a USB mode in which the device behaves like a flash drive when plugged into a PC, switchable from the screen
UI and from the web panel; video output over USB; every feature of mu300's kernel build config; progress on
mainline; the web installer identical to mu300's with our own options (the fulldumpdb target and the like); the
charging speed, which is "very bad", fixed to charge properly fast like mu300; and the idle load average looked at.

This document is what a builder plans from. `M/` is `/Volumes/TasinabilirSSD/projects/mu300-linux`,
`docs/MU300-PARITY.md` is the standing gap list (its item numbers A1..E3 are used below), the two recon reports of
2026-11-01 (mu300's USB and kernel config; mu300's updater, self-healing and all 308 merged PRs) are summarised in
the sections that use them.

## Intent

The U30 Pro port should feel like mu300-linux on the MU300: a device that updates itself safely, heals itself,
charges at the speed the charger allows, and is installed from a browser page that looks and behaves like
mu300's. On top of that, two things mu300 does not have: a USB flash-drive mode and USB host mode (for a
DisplayLink display later).

## Constraints that do not move

1. Linux stays a one-shot slot-b trial. Nothing makes Linux the default boot: no `mu300-next-boot` port, no
   `--rearm`, no `lock`, misc is only ever restored (`boot/init` at 1.6 s). An updater therefore has no boot
   counter to arm; its safety comes from writing only what can be undone and from the trial itself.
2. Slot a is never written. Modem NV partitions (`nr_*nv*`, `prodnv`) are never written; the modem gets RAM copies.
3. A full backup is taken before an install, as today; `fulldumpdb` and the region behind userdata are the two
   targets and both stay.
4. The device's SIM data is not used by any test the assistant runs. A feature that downloads on the device is
   fine; its test on this bench is the maintainer's to start.
5. `/sys/kernel/debug/clk/clk_summary` is never read (it hangs the kernel).
6. Releases are signed with the key only CI holds (`MU5358_RELEASE_KEY`); the page and the device refuse an
   unsigned release. The web installer's hard rules (no slot a, no NV, a password always) stay.
7. The kernel stays the GKI rebuild `6.6.98-android15-8-mu5358-4k`: every exported symbol's CRC equal to
   `kernel/Module.symvers` (the CI gate). A kernel option that changes a structure the vendor modules see is out;
   options that only add code or modules are in.
8. No test cuts the USB link to the bench Mac without an on-board self-undo (a timer that puts the port back).

## Hardware facts that shape the design (measured 2026-11-01 on the bench unit, in Linux)

- **USB:** `sprd,ums9632-musb`, USB 2.0 high-speed only, one HS PHY, no SuperSpeed lanes, no DisplayPort or HDMI
  controller in the SoC. **Type-C DisplayPort alt mode is impossible on this hardware**; so is any passive
  USB-C→HDMI adapter. mu300's own alt-mode experiment ended the same way.
- **Host mode is within reach:** `/sys/class/usb_role/64a00000.usb-role-switch` exists; the Type-C port reports
  `data_role: host [device]`, `power_role: source [sink]` under `sprd_tcpm`; the `vddvbus` OTG regulator is
  defined (disabled, no users); musb runs `b_peripheral`, `multipoint=true` (host code present). Untested:
  whether `musb_sprd` honours a role-switch write, whether `vddvbus` comes on with it.
- **Charging:** two bq2560x chargers (master `i2c 6-006b`, slave `4-006b`, vendor `sqc_bq2560x`/`sqc_charger`
  under `sprd_charger_manager` and `charger_policy_service`). On the bench the Mac offers 1.5 A (TCPM
  `current_max=1500000`), the master charges with IINLIM at the USB default 500 mA (`Reg00=0x04`), ~450 mA into
  the battery, 8 h to full; the slave has charging disabled (`Reg01=0x8a`). Every sysfs knob (`charger_psy`,
  `battery`, `interface` `input_current_limit`, `current_max`) accepts a write and changes nothing: in Android
  ZTE's userspace policy sets the limits over `sqc_netlink`/`zte_misc`. No `/dev/i2c-*`: `CONFIG_I2C_CHARDEV` is
  not set in the GKI config. The chip's watchdog is disabled (`Reg05=0x87`), so a register programmed from
  userspace is not undone by the chip itself. CV is 4.432 V (`Reg04=0x90`), ICHG register 3.12 A (`Reg02=0xb4`).
- **Load average 16 at 95 % idle:** sixteen vendor kernel threads sleep uninterruptibly forever
  (`rotation/0..7`, `native_hang_detect` ×2, `cpu_cdev0/1_update`, `aicwf_busirq_thread` ×2, `slog-0-0`,
  `shrink_anon`); context switches ~1400/s, interrupts ~900/s, no process uses CPU. It is a cosmetic number, but
  it hides a real load when one appears. `CONFIG_SPRD_ROTATION_TASK` is not in our kernel config, so the rotation
  threads come from a vendor module.

## Goals

- G1 An update from inside Linux and from Android, with the user's settings kept and a rollback, as safe as
  mu300's `mu300-update` within constraint 1.
- G2 The self-healing set mu300 has, where the hardware applies (A2..A12 of the parity list, plus what the PR
  list adds).
- G3 Charging at the current the source offers, on both chargers, with the guards mu300-power has.
- G4 A USB flash-drive mode, on/off from the screen UI and from LuCI, next to the USB network and console.
- G5 USB host mode on and off (with VBUS), and the LuCI/screen plumbing for it; DisplayLink output as a
  follow-on once a host-mode test passes and an adapter is at hand.
- G6 The web installer identical to mu300's in structure, flow and look, with the mu5358 options.
- G7 Every option of mu300's kernel fragments that the KMI gate allows.
- G8 Mainline past the USB bulk OUT stall.
- G9 The load average honest: the vendor D-state threads out where a module is optional, documented where not.

## Non-goals and anti-goals

- Not making Linux the default boot, not even as an option (constraint 1).
- Not porting mu300's LED, SC2355 Wi-Fi, SD card, Ubuntu/Cockpit, U30 Air/V50/F50 code (parity list section F).
- Not Type-C DP alt mode (hardware). Not a UVC "screen as a webcam" gadget (the maintainer did not want it).
- Anti-goal: a charging daemon that can overcharge or cook the battery. Limits come from the source's advertised
  current and the battery temperature; the daemon fails safe to the chip's own defaults.
- Anti-goal: an updater that can leave the device unbootable into Android. It writes `boot_b`/`init_boot_b` and
  files in the region only, reads back, keeps the previous images.

## The work, in eight streams

### W1 Web installer identical to mu300's (G6)

mu300's page: `M/site/assets/web-installer/` (app, flow, privacy, demo, device, manager, mirror, github,
keystore, zip, strings en/tr/zh, css), `M/docs/web-installer.md`, tests `M/tests/web/*.mjs` and
`M/tests/test_web_installer.py`. Ours already carries flow, privacy, demo, device, mirror, github, keystore,
strings, and adds ed25519/releasekey/sha512crypt. Missing by file: `manager.js` (the device manager over adb to
Linux: needs W5's adb) and `zip.js` (Magisk zips: not used here). The rest is a structural diff.

- Bring every panel, step, answer and wording of mu300's page over, in the three languages, keeping ours only
  where the device differs: the target choice (region behind userdata with the live f2fs shrink, or fulldumpdb),
  the full backup as a step the user sees, the password step, the one-shot trial explanation, the "nothing of
  slot a or NV is written" statement, and the mu5358 release list and mirror.
- The device manager (mu300 #244): once adb to Linux exists (W5), the page manages an installed device the way
  mu300's does (`mu300-api` → our `mu5358-api`).
- The device downloading the release itself (mu300 #223, `run fetch`): implement in `mu5358-install.sh` as
  mu300's `tools/android-web.sh` does (busybox wget, github.com hosts on 443 only, `take NAME SHA256` after),
  with the page offering it as mu300's does. Its bench test waits for the maintainer (constraint 4).
- Scope of "identical" (author's call): the page's panels, order, controls and wording; the installer takes
  the answers the device has a step for (password, hotspot name and password, language, target, VPN later
  with B8) and the page never shows mu300's systems/kernel/default-boot/attempts/Magisk/make-room-by-recovery
  panels: those are the expected absences the screenshot test lists.
- Tests: the string-equality and flow tests ported from `M/tests/web/`; a screenshot comparison of each panel
  against mu300's in Playwright is the acceptance check (same panels, same order, same controls).

### W2 Updates and the panel around them (G1)

What we have: `mu5358-install.sh update` from Android (the system written over the installed one, `CARRY` of
settings, `boot_b`/`init_boot_b`, downgrade refused), signed releases, the page's verification. What mu300 has
that we lack: an update from inside the running system, the LuCI Update page and rpcd adapter, the notice timer
and login note, settings backup/restore, user packages after an update, rollback.

Design, within constraint 1:

- **The layout** (author's call after the builder check): the region or fulldumpdb ext4 holds `system/` (the
  tree), `system.old/`, `data/` (what persists across updates: the flash-drive image, `prev.*` boot images,
  the carry area, logs) and `boot/` (`tree`: the name of the tree to boot, `tries`, `ok`). `boot/init` mounts
  the filesystem and switch_roots into `<fs>/<boot/tree>`; a missing tree, a failed manifest check, or
  `tries` past 3 without `ok` makes it boot `system.old` and say so on the persistent log. `ok` is written by
  a late service once the network is up (mu300's boot-ok, to a file in the region: misc is never touched).
  The release gains `mu5358-system.tar.gz` (the tree, ~50 MB) next to the image: both updaters unpack it into
  `system.new`; the image stays for a fresh install and carries the same layout. An existing flat install
  (v2026.10.30.x) is moved to the layout by the next update from the page/Android (`mu5358-install.sh update`
  learns the layout and stops erasing `data/`); the Linux-side updater refuses a flat install and says "update
  from the page once". Space: two trees (~300 MB each) fit fulldumpdb's 2 GiB; the flash-drive image's default
  is `min(2 GiB, free - 1 GiB)` and the feature is unavailable under 256 MiB free (W5).
- `mu5358-update` (POSIX sh, from `M/.../mu300-update` with the slot and misc code removed): `check`,
  `version`, `apply`, `report`, `rollback`, `clean`, `notice`, `source`, `status`, `releases`, `inspect`.
  - The system lives in one ext4 (region or fulldumpdb). Two trees inside it, `system` and `system.new` →
    `system.old`, swapped atomically; `boot/init` mounts the tree a marker names and falls back to `.old` when
    the named tree is missing or fails its manifest check. (The builder decides the marker and its fsync order.)
  - Download-everything-first, verified against the signed `SHA256SUMS` with the tag-binding line
    (`mu5358-release` = sha256("tag\n")), pinned sums, retries only on connect errors, a mirror source file,
    one run at a time under a 700 `/run` lock, room counted first, the self-update handover of the updater.
  - Carry: `M/tools/carry/mu300-carry.c` and `image-manifest.py` ported as `mu5358-carry`; the CARRY list of
    `mu5358-install.sh` becomes the manifest's "user" class so both paths keep the same things.
  - Boot images: `boot_b` and `init_boot_b` only, refused when Android's slot is b, `prev.*` kept in the region,
    sha256 read-back, `rollback-boot`. misc never.
  - Signed diffs (mu300 #416) are a later step: full images first.
- LuCI: `update.js` + the `update` adapter + rpcd methods, from mu300 with the kernel-choice and default-boot
  parts removed; CONFIRM words; upload a release file; progress stages; "Restart now" explains the trial boot.
- Notices: a procd timer (5 min after boot, then every 6 h) writing `/run/mu5358/update-available`; the login
  note; a badge on the u30ui2 dashboard (parity B10). Whether the check is on by default (one ~10 KB GitHub
  API call over the SIM) is the maintainer's (see "Theirs"); either way `/etc/mu5358/update-check off` turns
  it off and is carried.
- `mu5358-backup export|import` (mu300 #285) and the System > Backup page; the sysupgrade guard.
- `mu5358-user-packages` (mu300 #120/#447/#450).
- Tests: a fake-region test (`tests/fakeemmc`-style) for apply/rollback/rollback-boot, the hard-rule static
  checks extended to the new scripts (no `_a`, no NV, no misc writes).

### W3 Stability and self-healing (G2, G9)

From `docs/MU300-PARITY.md` and the PR list, the items that apply here, each a small service or a config line:

- A2 crash evidence: `collect-logs.sh` for Android (persistent log + sysdumpdb + uboot_log), `bootmark` lines,
  `panic_on_oops=1` from the ramdisk, `oops=panic panic=5` semantics checked against the PMIC watchdog.
- A5 `mu5358-power` (idle timers for Wi-Fi/radio/CPU, saver profiles, clean low-battery poweroff); the charge
  parts are W4's.
- A6 Wi-Fi: find the AIC8800 firmware-reset signature in dmesg, a hotplug/udev handler like `10-mu300-wcn`,
  `regdb-load`, the recovery-reboot backstop (mu300 #100).
- A7 mobile-data watchdog's dead-downlink ladder verified on `sipa_eth0` (restart VPN, redial, radio off/on,
  reboot ≤3 with the counter), logged to the persistent log before a reboot.
- A9/A10: see W5.
- A11 module autoload guard when GKI modules move to `/lib/modules`.
- A12 `boot/init`: `/run` 0755 nosuid nodev, eMMC wait, HS400ES retry if this eMMC shows it.
- Logs: logd 1 MiB (present), vendor log spam lowered (mu300 #90/#157/#320) for the vendor modules that take a
  parameter; `kmsg` warnings only.
- Time: NTP hotplug → traffic ledger `clock-synced` (present? verify).
- CPU: `mu5358-cpu` profiles (mu300 #111), CPU0 idle all-or-nothing (#465) if the governor here shows the issue.
- zram on (present, verify size/algorithm against mu300 #281).
- Load average: find which vendor module spawns `rotation/N` (`/proc/<pid>/stack`), `shrink_anon`, `slog-0-0`,
  `native_hang_detect`; blacklist what Linux does not need (Android's hang detector, the rotation task, the
  slog bridge when modem logging is off); document the rest in `docs/GKI-SYSTEM.md`. Target: load < 2 idle.
- mu300 PRs to read one by one for anything else: #435 (idle shell daemons spawn nothing), #465, #442, #452,
  #374, #302, #453, #466. The builder keeps a checklist in `docs/MU300-PARITY.md`.

### W4 Charging (G3)

- Kernel: `CONFIG_I2C_CHARDEV=m` (W6) gives `/dev/i2c-4` and `/dev/i2c-6`.
- `mu5358-charge` (C, static, small; or sh + i2cset if `i2c-tools` is in the image: the builder decides by what
  the rootfs has), a procd service, every 5 s:
  - reads the source: TCPM psy `current_max`/`usb_type` (C, PD, PD_PPS), BC1.2 type from `usb`/`ac` psy
    (SDP/CDP/DCP), the PD contract if any;
  - sets the master's IINLIM to the source's current (SDP 500 mA, CDP 1.5 A, DCP 2 A, Type-C per `current_max`,
    PD per contract, capped at 3 A); ICHG and the slave: slave charging enabled with ICHG split when the input is
    above 1.5 A, disabled below; VREG left at the chip's 4.432 V; termination current the chip's;
  - guards: battery temperature (from the fgu psy) windows like mu300's charge guard (stop charging above 45 °C,
    resume at 40; below 0 °C), the thermal-guard's say, an optional charge limit (`charge-limit N`, 50-100) that
    stops charging at N and lets it resume at N-3;
  - fails safe: on any I2C error or an unknown source it writes nothing and the chip keeps 500 mA; on exit it
    restores the chip's reset values for IINLIM (the vendor driver's 500 mA);
  - writes its state to `/run/mu5358/charge` (source, input mA, battery mA, temperature, limit) for u30ui2's
    battery page, LuCI's dashboard and `mu5358-power`.
- **What stock Android does on this unit, same cable, measured 2026-11-01 (Mac, Type-C 3 A advertised):**
  master IINLIM 1.5 A (`Reg00=0x0e`), ICHG register at its maximum, `charger_psy` input 1.5 A / constant 3 A,
  battery current 1.30 A at 23 %; the slave is held in Hi-Z (`Reg00=0x84`). So Android itself takes 1.5 A from
  a 5 V source and leaves the slave for a high-voltage fast-charge path (`sc27xx_fast_charger`, `ufcs`). The
  daemon's first target is Android's behaviour: 1.5 A from Type-C/CDP, 2 A from DCP, 500 mA SDP, the slave
  off; the slave and anything above 1.5 A only after the maintainer's own charger is measured in Android.
- Contention with the vendor driver: measure whether `sqc_charger` rewrites IINLIM (watch Reg00 for an hour after
  setting it); if it does, the daemon re-asserts every cycle and the finding is recorded.
- Acceptance on the bench: with the Mac (Type-C 3 A advertised), battery current ≥ 1.1 A at ~20 % charge, as
  Android's 1.30 A; with the maintainer's charger, what Android reaches on it (measured first); temperature
  never above 45 °C in a 1 h charge; the registers back to defaults after `mu5358-charge stop`.

### W5 USB: flash-drive mode, host mode, network mode, adb (G4, G5)

The gadget today: configfs composite (NCM + ACM) made in `boot/init`, `mu5358-usb` watchdog. All of this is in
the kernel already (`USB_CONFIGFS_MASS_STORAGE=y`, `F_FS=y`, `USB_ROLE_SWITCH=y`).

- **Flash-drive mode** (`mu5358-usb storage on|off|status|size N`):
  - a backing file in the region, `/mnt/data/usb-disk.img` or the fulldumpdb equivalent, created on first
    enable (default 2 GiB, exFAT, `size N` resizes when off; refused when the free space would drop under
    1 GiB), exported read-write as `functions/mass_storage.0/lun.0/file` in the same composite (`c.1/f3`), with
    `removable=1`, `nofua=1`, the device never mounting it while exported (the toggle unexports before any
    device-side mount; a `mount` helper for the device side when off);
  - the host sees "MU5358 Linux" vendor/product strings; bcdDevice changes so Windows re-reads descriptors;
  - adding or removing the function rebinds the gadget once (the NCM link drops and returns, as `mu5358-usb`
    already handles; the page/ssh user is told);
  - state in `/etc/mu5358/usb-storage` (on/off, size), applied at boot by `mu5358-usb boot`;
  - toggles: u30ui2 settings page (a row "USB flash drive: on/off", strings en/tr/zh via `i18n.h` +
    `mkfont.sh`), LuCI System > Device "USB" card (from mu300's `device.js` USB cards, plus this one), rpcd
    `usb_get/usb_set`.
- **Network mode** (parity A9): `mu5358-usb net ncm|ecm|rndis [once]` and `/etc/mu5358/usb-net` as mu300's,
  `boot/init` reading it; RNDIS with the MS OS descriptors (needs `USB_CONFIGFS_RNDIS`: W6 checks it builds
  as a module without touching KMI; the vendor `sprd_usb_f_rndis` is not used).
- **Host mode** (`mu5358-usb host|device`): write `host` to the role switch, enable `vddvbus`; the first test
  has a self-undo timer (back to device after 60 s unless `/run/mu5358/usb-host-keep` appears) and runs with
  the board on Wi-Fi; LuCI "USB role" card as mu300's; host-mode NICs enslaved to br-lan (mu300's
  `90-unisoc-usb-host`); USB storage/keyboards just work with the GKI drivers (`USB_STORAGE=y`, `USB_HID=y`).
  If `musb_sprd` ignores the role switch, the fallback is the TCPM `data_role` swap (works only with a PD
  partner) and the finding goes to `docs/GKI-SYSTEM.md`; the DisplayLink step then waits.
- **DisplayLink** (after host mode works and an adapter is at hand): `CONFIG_DRM_UDL=m` (W6) for DL-1x5
  adapters; evdi + DisplayLink's arm64 blob is out of scope until someone has such an adapter. A desktop is not
  in this design; the acceptance is a framebuffer console or u30ui2 mirrored on the HDMI screen.
- **adb to Linux** (A10, C7): adbd as FunctionFS in the composite (mu300's `tools/adbd`), default `keys`
  (authorized keys required, mu300 #150), `mu5358-usb adb on|off|auth`. Needed by W1's device manager.
- Tests: the gadget composition as a shell unit test with a fake configfs tree (mu300 has one), the toggles'
  state files, the LuCI rpcd methods with a fake `mu5358-usb`.

### W6 Kernel config parity (G7)

mu300's 5.4 `mu300-linux.fragment` (167 lines) and `upstream/mu300-mainline.config`'s module sets, filtered by
the KMI gate. Already in the GKI config: most cgroup/namespace/systemd options, the gadget functions, nftables
set (ours, patched), `BRIDGE_VLAN_FILTERING`. To add, as **modules** where possible so the vmlinux and its
CRCs do not move:

- `I2C_CHARDEV=m` (W4), `SPI_SPIDEV=m`, `DRM_UDL=m` (W5), `USB_CONFIGFS_RNDIS` + `USB_F_RNDIS` (check: in GKI
  the gadget functions are =y; a =y addition only adds code), `USB_SERIAL_*` (present), `USB_NET_RNDIS_HOST`,
  `QMI_WWAN`, `CDC_MBIM`, `USB_WDM`, `USB_PRINTER`, `SND_USB_AUDIO`, `USB_ACM`, AX88179/RTL8152 for host-mode
  NICs, `MACVLAN`, `IPVLAN`, `VXLAN`, `VLAN_8021Q`, `NET_SCH_CAKE`, `TCP_CONG_BBR`, `NET_CLS_BPF`,
  `NET_ACT_BPF`, `IP_SET` and its types, `NFT_*` not yet in ours (`NFT_OBJREF`, `NFT_SYNPROXY` is out by the
  conntrack-extension rule), `WIREGUARD`, `BTRFS`, `XFS`, `SQUASHFS` (+xz/zstd/lz4), `ISO9660`, `UDF`, `NTFS3`,
  `EXFAT` (check), `CIFS`, `NFS`/`NFSD`, `ZRAM` (check), `CRYPTO_USER_API_*`, `TLS`, `TUN` (check), `VETH`,
  `BRIDGE` (present), `IKHEADERS=m`, `IKCONFIG_PROC`.
- Rejected by rule: anything adding fields to `struct net`, `struct sock`, `struct task_struct`,
  `struct mm_struct`, cgroup controllers not already on (`CGROUP_HUGETLB` adds a subsys id), `AUDIT`,
  `SECURITY_APPARMOR` (LSM slots), `MODULE_SIG_FORCE` off (we do not sign modules: check the stock setting),
  `UEVENT_HELPER`, `CHECKPOINT_RESTORE` if it changes `task_struct`.
- Process: one fragment change at a time in CI (`kernel/build.sh` + the CRC gate); `kernel/fragment-rejected.txt`
  records what olddefconfig refused and what the gate refused and why. The modules land in
  `mu5358-tools.tar.gz`'s module set and `/lib/modules` (W3 A11 then applies).
- The D-state vendor threads: the rotation task and friends are vendor modules, not this fragment (W3).

### W7 Mainline (G8)

`mainline/` is paused at USB bulk OUT on 6.18.59: the Unisoc MUSB needs its DMA engine (`sprd_musbhsdma`) for
OUT endpoints. The step: port the vendor `musb_sprd` DMA glue (the 5.4 GPL tree mu300 uses has it:
`M/kernel/...` and Enceka's UMS9620 tree) onto mainline's `musb_dma.h` interface as `drivers/usb/musb/sprd_dma.c`,
behind a patch in `mainline/patches/`; acceptance: NCM carries 50 MB each way without a stall, ACM console
survives it; then the next blocker is recorded in `docs/GKI-SYSTEM.md`'s mainline section. Time-boxed: two
days; if the DMA glue does not come together, the finding and the next hypothesis are written down and the
stream stops for this round.

### W8 CI and release chain

- The Release workflow (done 2026-11-01) runs its first release now; whatever it fails on is fixed first.
- A weekly kernel build (`kernel.yml` cron) that builds the GKI with the fragment and runs the CRC gate: early
  warning for W6 changes and for ACK bumps if we ever move the commit.
- Promotion stays manual (`gh release edit TAG --prerelease=false --latest`) after a bench trial boot.
- Telegram announcements (mu300's `telegram.yml`): only if the maintainer wants a channel and a bot token.

### W9 VPN: mu300-vpn (asked 2026-11-01, "Mu300-vpn desteği de getir")

mu300's VPN is a module of its own (`dikeckaan/mu300-linux-vpn`, parity B8): `mu300-extra install vpn` fetches it
into the extras directory on the disk, `mu300-vpn` drives the engines (xray, sing-box, mihomo, dae; profiles,
subscriptions, uplink hotplug `96-mu300-vpn-uplink`, the fail-closed kill switch `lib/vpn-orphan.sh`), and the
LuCI VPN pages (overview, profiles, subscriptions, engines, logs, settings) come with the control panel we already
carry (renamed by `openwrt/rebrand.sh`; hidden while no module is installed, mu300 #182).

- The extras directory is `data/extra` of W2's layout (mu300: `/mnt/mu300-disk/extra`); `mu5358-extra` is
  mu300's `mu300-extra` renamed, with the disk path and the module's release repository as its two differences.
  The module itself (the engines' arm64 binaries) is mu300's release, verified by its signed sums, not rebuilt
  here: nothing in it is board-specific (the kill switch and uplink scripts name `sipa_eth0` through
  `mu5358-device`, A1 of the parity list).
- The download runs on the device (the SIM, or the USB host's connection when the device is a client): the page
  and LuCI say the size first (xray ~30 MB); the bench test is the maintainer's to start (constraint 4).
- Acceptance: a WireGuard profile (in the image already) and one xray profile the maintainer supplies, traffic
  from a Wi-Fi client through the tunnel, the kill switch dropping traffic when the engine is stopped, the LuCI
  pages in en/tr/zh.
- Order: after W2 (needs `data/`); size M.

## Order and size

| Order | Stream | Size | Why here |
|---|---|---|---|
| 1 | W8 first release through CI | S | everything else ships through it |
| 2 | W6 kernel fragment round 1: `I2C_CHARDEV`, `DRM_UDL`, RNDIS, spidev | S | W4 and W5 need the modules; one CI kernel build |
| 3 | W4 charging | M | the maintainer's most felt problem; measurable on the bench |
| 4 | W5 flash-drive mode + network mode + toggles | M | asked explicitly; no new kernel work |
| 5 | W2 updater from Linux + LuCI Update + notices + backup | L | the core of "as good as mu300" |
| 6 | W1 web installer identical to mu300 | M | needs W5's adb for the manager; the rest can start earlier |
| 7 | W3 stability items, load average | M | many small ones; interleaved with 3-6 as bench time allows |
| 8 | W5 host mode test, then DisplayLink | S + M | needs the maintainer's hands (OTG adapter, Wi-Fi) |
| 9 | W6 round 2 (the rest of the list) | M | each a CI build and a trial boot |
| 10 | W9 VPN (mu300-vpn module, extras in `data/extra`) | M | after W2's layout; the engines are mu300's release |
| 11 | W7 mainline DMA | L (time-boxed) | nothing else depends on it |

Each stream is its own plan (`superpowers:writing-plans`), its own branch and PR, tests first, a trial boot on
the bench before the PR merges when it touches the device.

## Decisions made here (the maintainer's)

- The web installer is mu300's page, not a variant: same panels, flow, wording, with the mu5358-only options.
- Video over USB: not UVC; DisplayLink after host mode, and only if an adapter exists. Type-C DP is impossible.
- Charging must reach the source's current; both chargers.
- Flash-drive mode switchable from the screen UI and LuCI.
- Everything through CI; nothing run by hand on the Mac for a release.

## Calls made by the author (change them if wrong)

- Flash-drive backing store: a 2 GiB exFAT image file in the region, resizable, never mounted on the device
  while exported.
- adb to Linux ships **on with keys required** (mu300 #150), not open.
- The updater keeps two trees in the one filesystem (`system`, `system.old`) instead of a second partition.
- Charge guard windows: stop above 45 °C, resume at 40 °C; charge limit default 100 (off).
- Host-mode first test: 60 s self-undo.
- Telegram: not set up unless asked.
- W7 is time-boxed to two days this round.

## Theirs (asked and answered 2026-11-01)

1. The update check over the SIM: **on by default**, as mu300's; `/etc/mu5358/update-check off` turns it off
   and is carried. The bench SIM may make this call.
2. DisplayLink: **none at hand**; this round's USB display work ends at a passing host-mode test.
3. The charger in use: **a Mac mini M4 Type-C port** (PD-capable). Android takes 1.5 A from it (measured above):
   that is `mu5358-charge`'s target; the slave charger stays off this round.
4. Telegram: **yes**. mu300's `telegram.yml` + `tools/telegram-announce.sh`, English, token via `curl -K`; the
   maintainer adds the repository secrets `TELEGRAM_BOT_TOKEN` and `TELEGRAM_CHAT_ID` (W8).

## Left to the builder

The marker and fsync order of the tree swap; the daemon language for `mu5358-charge`; exact LuCI card layout
(copy mu300's); the module list of W6 round 2 after the first CI build shows what olddefconfig and the CRC gate
accept; the fake-configfs test harness; how the u30ui2 settings page grows a row without breaking its font
table (`i18n.h` + `mkfont.sh` is the only way to add strings).
