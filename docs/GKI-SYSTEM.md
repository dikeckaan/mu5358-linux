# The Linux system on the U30 Pro (stock GKI 6.6 + OpenWrt)

Linux boots from slot b as a one-shot trial (boot/init puts misc back to Android's block at 1.6 s), never as the
default: every reboot or power-off ends in Android unless the trial is armed again. `openwrt/customize.sh` makes all of
the below on the root filesystem (from Android: `customize.sh ROOT /vendor_dlkm/lib/modules /odm/firmware/aic8800
/mnt/vendor`).

## Services (OpenWrt init)

| | Start | What |
|---|---|---|
| mu5358-screen | S11 | the screen: u30ui2, the dashboard (`tools/u30ui2`; EN/TR/ZH); u30ui, the launcher's papatya, when `/etc/mu5358/ui` says classic; else the log (`tools/screencon`) |
| mu5358-hw | S12 | vendor modules (thermal, DVFS, zram, Wi-Fi), zram, then the hotspot |
| mu5358-log | S13 | the system's state into the persistent log on the eMMC, every 5 s, after 5 min every minute |
| mu5358-modem | S15 | modem_control, cp_diskserver, refnotify in their chroot; NV only as RAM copies (`modem/README.md`) |
| mu5358-accounts | S11 | mu300's: root's password and the web login kept in step |
| mu5358-atd, mu5358-atd-dash | S19 | mu300's AT daemons (nr1 commands, nr2 data; nr6/nr7 for the panel) |
| mu5358-smsd | S95 | mu300's SMS pool (the panel's messages, `mu5358-sms`) |
| mu5358-traffic, mu5358-toolkit | S96 | mu300's traffic ledger (per day and billing cycle); TTL/TCP settings applied at boot |
| mu5358-thermal | S99 | mu300's thermal-guard, reading only the zones that are sensors (frame-thmzone reads 86 C on a cool board) |
| mu5358-user-packages | S99 | mu300's: the user's own package list |
| network | S20 | wan = mu300cell over sipa_eth0 (APN `internet`) |
| mu5358-buttons | S95 | power: short = screen on/off; held 1.5 s = the dashboard's Power page (its hold ring confirms restart / power off), held 3 s = power off when the dashboard is not the screen (classic UI, console, or no u30ui2 process); Wi-Fi key held = hotspot off/on (the NFC tag re-synced) |
| mu5358-nfc | S95 | NFC tap-to-connect: `mu5358-nfc-sync` writes the hotspot's Wi-Fi credential into the NFC tag at boot, after a LuCI apply (reload trigger), the AP's ifup/ifdown (hotplug), every `wifi reload` and the switches (`/etc/mu5358/nfc off`; the screen's Wi-Fi page, the panel's Device card) |
| mu5358-cell | S99 | mu300's `mobile-data watch` (redial after two failed checks) |
| (panel) | - | mu300's LuCI panel as `luci-app-mu5358` (ubus `mu5358dash`), the Aurora theme, ttyd: `http://192.168.58.1/cgi-bin/luci/admin/home`, root's password |
| mu5358-usb | S99 | re-enumerates the USB gadget (5 s off) while no host has it: not configured, no carrier, or our TX still while the host's packets come in |
| mu5358-boot-ok | S99 | 30 s into a working system: `boot/ok` and `boot/tries` = 0 on the filesystem (boot/init boots `system.old` after three boots without it); a fallback boot is made permanent (the failed tree kept as `system.failed`); misc is never touched |
| mu5358-update-notice | S99 | 5 min after boot and every 6 h: `mu5358-update notice` asks GitHub for the newest release (one small API call; `/etc/mu5358/update-check off` stops it) and writes `/run/mu5358/update-available` for the login note, the panel and the screen |

mu300-post is not used: it makes the running slot the default boot (`mu300-next-boot --rearm`); mu5358-boot-ok is its
bootok without the re-arm.

## The filesystem and updates (2026-11-02, W2)

The system lives in one ext4 (the region behind userdata, or fulldumpdb) in mu300's layout: `system/` (the tree),
`system.old/` (the one before the last update), `data/` (bind-mounted at `/data`: the flash-drive image, the previous
boot images in `data/prev/`), `boot/` (`tree`: the name of the tree to boot, `tries`, `ok`, `installed`: the record
of the boot images' hashes that the Android installer and `mu5358-update` share). boot/init mounts the filesystem at
`/disk`, bind-mounts the tree as the new root (the whole filesystem at `/mnt/mu5358-disk`), adds 1 to `boot/tries`,
and after three boots without `boot/ok` for that version boots `system.old` (`stage=tree-fallback` in the persistent
log). An installation from before (the tree at the top) still boots (`stage=tree-flat`) and is moved to the layout
by its next update from the page (`MU5358-STEP migrate`: renames only, the flash-drive image kept).

Updates run in three ways, all with the settings carried (`/etc`, root's keys, the screen's choices; the installer's
`CARRY` list and the carry's user-area rule are held equal by a test):

- From the web installer (Android): `mu5358-install.sh update` unpacks the release's `mu5358-system.tar.gz` into
  `system.new`, puts this device's vendor files and the carried settings in, swaps the trees, writes `boot_b` and
  `init_boot_b`. The whole filesystem is never written on an update.
- From inside Linux: `mu5358-update check|apply|rollback|rollback-boot|clean|report|status` (mu300-update's port,
  `openwrt/update/mu5358-update`: the slot re-arm, the kernel bundles and the misc code are gone). `apply` downloads
  everything first (SHA256SUMS signed and bound to its tag by the `mu5358-release` line, pinned for the run),
  unpacks into `system.new`, carries the user's files with `mu5358-carry` (mu300's static tool from the base
  rootfs; its manifest header keeps mu300's name), swaps, then builds the boot images exactly as the installer does
  (the shared `bootimg` block; the ramdisk packed LZ4-legacy with the static `lz4` of `tools/lz4/build.sh`, the
  ramdisk's busybox, misc block and modules published by boot/init at `/run/mu5358/ramdisk`) and writes them with a
  read-back, the previous ones kept in `data/prev`. Refused off slot b, on a flat installation, and for a downgrade
  without `--downgrade`. The panel's System > Update page drives the same commands (no kernel chooser, no default-boot
  switch here).
- A restart after any update starts Android: Linux is the one-shot trial, and it is started again from the page
  ("Restart into Linux once"). The page learns a Linux-side update from `boot/installed` before `status` and
  `boot-linux` (the record is read key by key, never as a file; the record whose `boot_b` and `init_boot_b` are
  the live partitions' wins, the date only breaks a tie; the filesystem is mounted nosuid,nodev and no link in it
  is followed).

**The bench run of 2026-11-02 (v2026.11.01.5 → .02.1 → .02.2 → .02.3, the rollback, the fallback).** From Android
(the installer's `update` with the tree): the migration, the swap, the boot images and the trial boot in 1 min 43 s;
the flash-drive image and the adb key kept. From Linux (`mu5358-update apply`, the release served to the device over
the USB link from the Mac - `MU5358_RELEASE_URL`/`MU5358_API_URL` - so the SIM carried nothing): the self-update
handover to the release's own updater, the download and signature, the carry with the manifest (549 files, the
device's vendor files and settings), the swap and the boot images with their read-back in 20 s; Android took the
record and `boot-linux` started the new tree; `rollback` put the previous tree and the previous boot images back
(verified by a boot); three boots without `boot/ok` made boot/init boot `system.old` (`stage=tree-fallback tries=3`)
and boot-ok made that permanent (the failed tree kept as `system.failed`, removed by `clean` and the next `apply`).
What the device taught on the way, each fixed with a test: OpenWrt has no `stat` and no `/dev/block/by-name` (the
shared boot-image block uses `wc -c`; the updater finds the partitions by their GPT name in sysfs), the GKI kernel
keeps `androidboot.slot_suffix` in `/proc/bootconfig`, the carry's manifest header names mu300's image
(`openwrt-luci`), and two releases with the same kernel share `boot_b` byte for byte (so `init_boot_b` tells the
records apart). A fallback boot runs the previous tree on the newer release's boot images (the ramdisk is
self-contained); `rollback-boot` puts the previous images back if wanted.

## Hardware status (2026-10-30)

| | |
|---|---|
| eMMC, USB (NCM/ECM/RNDIS + ACM, a flash drive on demand, adb with keys) | yes (the flash drive and adb: 2026-11-01, `openwrt/usb`; the drive any size, formatted from the computer if wished; see the USB findings below) |
| Modem, mobile data | yes: Turk Telekom LTE; 14-19 Mbit/s at this place, Android 11-12 on the same spot |
| Wi-Fi | AIC8800D80X2: hotspot 5 GHz channel 36, HE80 (Wi-Fi 6), WPA2; the board's MAC and calibration |
| Screen | u30ui2: status bar + home (live graph), network, Wi-Fi (with the NFC switch), clients, traffic, SMS, battery, system, settings, power, log in three languages; wakes on the charger's uevent (a plug or unplug: `usb` and the TCPM source supply announce it on this kernel, `ac` never did; battery's events are ignored, nothing polls); the power key held opens the Power page through the lock; the first frame is in the buffer before the mode-set (no black frame at the start); a host simulator and a seeded fuzz under ASan drive the real `ui.c` in the tests (`tools/u30ui2/sim.c`, `tests/test_u30ui2.py`); 6 MB RAM, 0.5 % CPU screen on, 0.1 % off (u30ui, the papatya, kept as the classic interface) (no fbcon in GKI: DRM dumb buffers, PAGE_FLIP; the panel is mounted upside down and turns the picture itself, MADCTL through `tools/spispeed/panelcmd`: turned by the CPU it tore) |
| GPU | Mali-G57 (arch 9.0.9, DDK r49p1), OpenGL ES 3.2 / EGL 1.5 / Vulkan through Android's libGLES_mali.so in the chroot, on demand (`gpu/README.md`); no OpenCL in Android 15's driver. 60 s stress: 950 MHz, ~100 frames/s of a heavy shader, 40 °C, idle back at 384 MHz and powered off |
| Buttons | power, Wi-Fi key (volume keys are in the DT, not used) |
| Battery, charger | vendor charger-manager; `mu5358-charge` sets the input limit from the source (2026-11-01: 1.5 A from the Mac mini's Type-C port, 1.25-1.37 A into the battery, 55 → 83 % in one hour at 32-33 °C, the vendor driver never rewrote it; before: 500 mA, 450 mA into the battery); at 100 % the battery idles (-5 mA) and the system runs on USB |
| LEDs | none on this board |

## Known behaviour

- **An oops restarts the board** (`kernel.panic_on_oops=1`, `kernel.panic=10`), into Android. On 2026-10-30 an oops
  that died holding the RTNL lock left a board with Wi-Fi and the screen up but every network command hung.
- **Never unbind the USB controller** (musb-sprd unbind/bind): usb0 lives on with its parent device freed, and the
  next link change oopses in rtnl_fill_ifinfo (work/logs/rtnl-oops-2026-10-30.txt). Only the gadget's UDC is rebound.
- mu300's LAN hook (hotplug.d/iface/10-mu300-usb) re-enumerated the gadget at every LAN ifup; it broke a working link
  that a macOS host then never took up again until the cable was pulled. customize.sh makes it skip a link with carrier.

- **Power off with USB connected starts the board again** (LK: "Reboot into normal"; the fuel gauge counts the time it
  was off): the PMIC powers up on VBUS. Without a cable it stays off. Either way the next boot is Android.
- The vendor musb keeps reporting `configured` after the host is gone: mu5358-usb looks at the NCM carrier too.
- **The role switch takes no userspace writes:** `/sys/class/usb_role/64a00000.usb-role-switch` has no `role`
  attribute (the vendor driver registers it without `allow_userspace_control`), so host mode cannot be asked of it.
  **The port's policy can (2026-11-03, from kanoqwq/u30pro-bringup):** `/sys/class/typec/port0/port_type` takes
  `dual` (the default: the attachment decides), `source` (host, VBUS from the boost regulator) and `sink` (device);
  TCPM detaches and re-evaluates. `mu5358-usb auto|host|device` writes it (`host` undoes itself after 60 s unless
  `/run/mu5358/usb-host-keep` exists; the panel's "USB role" card does the same), the data-role swap stays the
  fallback on a kernel without a writable `port_type`. `status` also reports `musb_hostenable` (`enabled` here,
  report-only). Bench with the Mac's cable, three guarded trials: `auto` and `device` kept the link; `host` dropped
  it for 5 s and the port re-attached as a peripheral (the computer is the source), the undo put `dual` back at 60 s.
  The OTG test (an adapter and a USB stick in place of the cable, the board on its hotspot) is still the maintainer's.
- **The USB composite is composed from the state files by `boot/init`** before switch_root (`/etc/mu5358/usb-net`,
  `/etc/mu5358/usb-storage`): a mode never costs a rebind at boot. The flash drive and adb rebind the gadget once when
  they are toggled while plugged in (the computer sees a replug); `mu5358-usb boot` never rebinds.
- **Bench, 2026-11-01 (the flash drive and adb, the system updated by hand before the release):** `mu5358-usb storage
  on` while the Mac held the link: the gadget off and on again, ssh back within 10-12 s, the Mac shows the disk
  (`diskutil`: external, removable, FAT32 "MU5358", 2.1 GB; `idProduct 0xa4a2`); a 100 MB file written from the Mac
  at 30 MB/s read back on the device with the same sha256 (`storage mount`), and the disk back on the Mac 2 s after
  `storage umount` (a medium change, no replug). A function cannot be linked into the configuration while the gadget
  is bound (`ln: Invalid argument`): the tool unbinds, changes, binds. adb: the Mac's key authorised with
  `mu5358-usb adb key -`, `adb devices` says `device` and `adb shell id` is root; a computer without the key sees
  `unauthorized` (keys mode). The adbd service's rebind cost the link 12 s. `/etc/mu300` is a symlink to
  `/etc/mu5358` on the image, which is how mu300's adbd finds `/etc/mu5358/adb_keys`. The gadget's serial string
  was `MU5358LINUX` without the unit's number on this boot (`androidboot.serialno` not in the command line).
  The computer's own "eject" takes the medium out of the removable LUN (`lun.0/file` goes empty; `storage status`
  then says `exported: no` while the drive is on); `mu5358-usb storage on` puts it back without a replug (the Mac saw
  the disk again 2 s later), as does the next boot.
- **Release v2026.11.01.2 on the bench (trial boot, 2026-11-01):** with `usb-storage` "on" carried by the update,
  `boot/init` composed the disk before switch_root (`stage=usb-apply net=ncm disk=1 rebind=1`: the early NCM
  gadget rebuilt once in the initramfs, nothing rebound by the system), `mu5358-usb boot` put the image in at 31 s,
  the Mac listed the disk at 45 s - the same moment ssh answered - and `adb devices` showed the board with the
  carried key. `/run` is a plain directory on the root filesystem in this image (only `/tmp` and `/var/run` are
  tmpfs), so `/run/mu5358/*` outlives a reboot: the tools treat its markers accordingly (a keep file made before a
  `host` swap does not count). The installer's "a job is running" check now also asks whether the pid is this
  script: the job's pid file outlived a reboot and its number belonged to another process, which blocked an update.
- **Release v2026.11.01.3 on the bench (trial boot, 2026-11-01, "a real USB stick: format it from the computer, make
  it big"):** the charger set at boot (`source=typec input_ma=1500 reg00=0x0e`, the battery at 100 % idling at -4 to
  +2 mA, 30.9 °C, `tcpm` the source). The Mac formatted the drive as it pleased (`diskutil eraseDisk ExFAT MU5358 GPT`:
  a GUID table and an exFAT partition); the device's `storage mount` reads the table (MBR or GPT; `od` from the
  shipped busybox, the rootfs has only `hexdump`) and mounts the partition with `offset=` (busybox mount does loop
  offsets; the kernel has `exfat`, `vfat` and `ntfs3`): a 20 MB file written from the Mac read back with the same
  sha256, the disk back on the Mac after `umount`. `storage size 16384` made a 16 GiB sparse FAT32 image in a moment
  (8 MiB used); `storage on` brought the link back in 20 s and the Mac mounted a 17.2 GB "MU5358" (`storage size`
  takes anything up to the free space less 1 GiB; the default stays 2 GiB).
  **Two findings in `usb-watch` on the way** (fixed the same day, `openwrt/usb/usb-watch`, tests
  `tests/test_usb_watch.py`): (1) one bad sample was a re-enumeration - the first `storage on` of the 16 GiB disk was
  cut short at 19 s while the Mac was still enumerating, the Mac started over, was cut again at 60 and 120 s, adbd's
  own rebind collided ("could not bind musb-hdrc.1.auto again") and the link never settled; the armed self-undo
  rebooted the board to Android after 300 s as designed. The watch now acts only on a state that has lasted 60 s and
  never within 90 s of a bind the tools did on purpose (the uptime in `/run/mu5358/usb-rebound`). (2) The "sending
  stalled" rule (TX still for 60 s while RX grew) fired on a working link 110 s after boot: the Mac's mDNS kept RX
  growing and the device owed it nothing. Before calling a sender dead the watch now pings the DHCP lease holder once
  and looks at TX again. The 300 s reboot self-undo (`( trap "" HUP; sleep 300; [ -e keep ] || reboot ) &`) is the
  pattern for any rebind trial on the bench: the trial boot returns to Android, where the region can be mounted
  (`losetup -o 33382465536` on this unit, mksh arithmetic is 32-bit so the byte offset is written out) to turn the
  drive off in `etc/mu5358/usb-storage` before the next `boot-linux`.
  **Release v2026.11.01.4 on the bench (trial boot, 2026-11-01):** the 16 GiB drive composed by `boot/init`, ssh at
  17 s of uptime, the Mac's 17.2 GB disk mounted, the charger set (`input_ma=1500`), and the patient `usb-watch`
  quiet through the first three minutes (no "re-enumerating" line; the old one had fired at 110 s).
- **The web installer, end to end from the browser (2026-11-01, Chrome on macOS, the bench unit in Android):** the
  page connected over WebUSB, fetched v2026.11.01.5 from the mirror (65 MiB), checked SHA256SUMS and its signature,
  pushed the script, read the device's state, updated the installed v2026.11.01.4 with the settings carried (37
  files), checked the trial boot dry, and started Linux once: ssh answered 30 s after the click, `adb_keys` and the
  16 GiB `usb-storage` were still there. Found and fixed on the way: (1) `status` read fulldumpdb's 2 GiB whole,
  twice, to say "empty" - 73 s on the bench against the page's 60 s, and the panel sat on "Reading the device's
  state" for good; now five MiB are sampled (2 s) and a failed read shows the error with "Ask again". (2) The panel
  said "Install again (update)" but ran `install`, which writes the system anew without the carry; `jobKind` in
  flow.js now picks the script's `update` (no password) where a system is installed. (3) Chrome kept the device
  claimed for three old tabs of the page in other windows ("another program is using the device"; `ioreg` showed
  Chrome's user clients on the ADB interface, and even `adb devices` lost the device): close them. (4) The first
  connection right after `adb kill-server` failed once with "the device went away" (NotFoundError on the retry's
  open); the page's reconnect on the USB connect event took over. (5) The mirror branch answered 404 for about a
  minute after the release was published (raw.githubusercontent's cache): "try again" is right. The job's follow
  stream showed the last lines (boot step, exit) only after a status read; the job itself had finished.
- Load average ~16 while idle: vendor kernel threads in D state (as on mu300, FINDINGS 31e).
- **Charging stays at 500 mA without Android** (2026-11-01). Two bq2560x chargers (master i2c 6-006b, slave 4-006b,
  vendor `sqc_bq2560x`/`sqc_charger` under `sprd_charger_manager`). In Android ZTE's userspace policy
  (`charge_arbitrate_service`, over `sqc_netlink`/`zte_misc`) sets the input limit: 1.5 A from the Mac mini's Type-C
  port (`Reg00=0x0e`), 1.30 A into the battery, the slave in Hi-Z. In Linux the master kept the USB default
  (`Reg00=0x04`, 450 mA into the battery, 8 h to full) and every sysfs knob (`charger_psy`, `battery`, `interface`
  `input_current_limit`/`current_max`) accepted a write and changed nothing. `mu5358-charge` (tools/charge) programs
  REG00/REG01 of the master over `/dev/i2c-6` (this kernel's i2c-dev) from the TCPM/BC1.2 power supplies; the chip's
  watchdog is off (`Reg05=0x87`), so a value stays until rewritten. The slave is never touched.
- Boot-time WARNINGs that are harmless: device_links (ums9632_clk), sipa_eth0 "Incorrect netdev->dev_addr".

## NFC tap-to-connect (2026-11-03)

The U30 Pro's NFC tag is a Fudan FM11NT08x on `/dev/i2c-4` at 0x57 (device tree node `ntag_fm11_gpio`; the stock
`tmsnfc` module only holds its reset and interrupt lines, nothing is bound to the address, `i2c-dev` and
`i2ctransfer` are in the image). It is an NFC Forum Type 2 tag: capability container `e1 10 6d 00` (872 bytes of
NDEF data, read/write), SAK `00` on the bench unit (a phone reads it as a Type 2 tag; the factory value 0x20 would
make phones talk Type 4 and see nothing, the tool rewrites it). The stock firmware kept a Wi-Fi Simple
Configuration credential there; `mu5358-nfc` (tools/nfc, from kanoqwq/u30pro-bringup's u30pro-nfc, origin
33333s/U30pro-openwrt, MIT) writes the current hotspot the same way: an NDEF record of type
`application/vnd.wfa.wsc` with the SSID, the authentication (open or WPA2-Personal: `psk2*` and `sae-mixed*`; a pure
SAE, OWE, WPA1 or enterprise hotspot leaves the tag empty, phones take only those two from a tag) and the key. Pages
of 4 bytes, only the ones that differ, the NDEF length page emptied first and committed last (a power cut never
leaves half a credential), the whole message read back. The key reaches the tool on stdin only; `show` prints the
SSID's length (`show -v` the SSID), never the key.

`mu5358-nfc-sync` (openwrt/nfc) is the one writer, serialised by `flock`: `sync` (boot, the LuCI apply through the
reload trigger, the AP's ifup/ifdown, every `wifi reload` in `buttons` and the screens), `on|off|toggle`
(`/etc/mu5358/nfc`, carried across updates), `state`, `supported`. The result word is in `/run/mu5358/nfc-result`
(`open wpa2 disabled hotspot_off unsupported no_tag error`), read by the screen's Wi-Fi row and the panel's card
(rpcd `mu5358nfc`). Tests: the tool against a file-backed i2c device under ASan (an independent WSC encoder as the
oracle, an interrupted write, a read-only tag, the SAK), the script under busybox with `uci` and the tool stubbed.
The phone tap itself is the maintainer's check.

## Where the logs are

- The persistent log: sectors 122152960.. of mmcblk0 (1 MiB; boot/init's kernel log in the first half, mu5358-log's
  snapshots in the second). From Android: `dd if=/dev/block/mmcblk0 bs=512 skip=122152960 count=2048`.
- LK's log: the uboot_log partition (boot cause: `BootCause is ...`).
- `logread -e mu5358-nfc`: the tag's failures (never the SSID or the key); `logread -e mu5358-usb`: the host mode's undo.
- The tree chosen at boot: `stage=tree-mounted tree=system tries=N`, `stage=tree-fallback ...`, `stage=tree-flat` in the
  persistent log's stages (W2).
- A boot that never reaches userspace: sysdumpdb (LK minidump), `tools/sysdumpdb-extract.py`.
