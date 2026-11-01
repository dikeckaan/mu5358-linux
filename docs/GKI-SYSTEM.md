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
| mu5358-buttons | S95 | power: short = screen on/off, held 3 s = power off; Wi-Fi key held 3 s = hotspot off/on |
| mu5358-cell | S99 | mu300's `mobile-data watch` (redial after two failed checks) |
| (panel) | - | mu300's LuCI panel as `luci-app-mu5358` (ubus `mu5358dash`), the Aurora theme, ttyd: `http://192.168.58.1/cgi-bin/luci/admin/home`, root's password |
| mu5358-usb | S99 | re-enumerates the USB gadget (5 s off) while no host has it: not configured, no carrier, or our TX still while the host's packets come in |

mu300-post is not used: it makes the running slot the default boot (`mu300-next-boot --rearm`).

## Hardware status (2026-10-30)

| | |
|---|---|
| eMMC, USB (NCM/ECM/RNDIS + ACM, a flash drive on demand, adb with keys) | yes (the flash drive and adb: 2026-11-01, `openwrt/usb`; see the USB findings below) |
| Modem, mobile data | yes: Turk Telekom LTE; 14-19 Mbit/s at this place, Android 11-12 on the same spot |
| Wi-Fi | AIC8800D80X2: hotspot 5 GHz channel 36, HE80 (Wi-Fi 6), WPA2; the board's MAC and calibration |
| Screen | u30ui2: status bar + home (live graph), network, Wi-Fi, clients, traffic, SMS, battery, system, settings, power, log in three languages; 6 MB RAM, 0.5 % CPU screen on, 0.1 % off (u30ui, the papatya, kept as the classic interface) (no fbcon in GKI: DRM dumb buffers, PAGE_FLIP; the panel is mounted upside down and turns the picture itself, MADCTL through `tools/spispeed/panelcmd`: turned by the CPU it tore) |
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
  `mu5358-usb host` asks the Type-C port controller instead (`/sys/class/typec/port0/data_role`, a USB-PD data role
  swap, which needs a partner that speaks PD) and undoes itself after 60 s unless `/run/mu5358/usb-host-keep` exists.
  Untested until the OTG bench test (an adapter and a USB stick in place of the Mac's cable, the board on its hotspot).
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

## Where the logs are

- The persistent log: sectors 122152960.. of mmcblk0 (1 MiB; boot/init's kernel log in the first half, mu5358-log's
  snapshots in the second). From Android: `dd if=/dev/block/mmcblk0 bs=512 skip=122152960 count=2048`.
- LK's log: the uboot_log partition (boot cause: `BootCause is ...`).
- A boot that never reaches userspace: sysdumpdb (LK minidump), `tools/sysdumpdb-extract.py`.
