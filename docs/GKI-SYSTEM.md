# The Linux system on the U30 Pro (stock GKI 6.6 + OpenWrt)

Linux boots from slot b as a one-shot trial (boot/init puts misc back to Android's block at 1.6 s), never as the
default: every reboot or power-off ends in Android unless the trial is armed again. `openwrt/customize.sh` makes all of
the below on the root filesystem (from Android: `customize.sh ROOT /vendor_dlkm/lib/modules /odm/firmware/aic8800
/mnt/vendor`).

## Services (OpenWrt init)

| | Start | What |
|---|---|---|
| mu5358-screen | S11 | the screen: u30ui, the launcher's papatya natively (`tools/u30ui`); else the log (`tools/screencon`) |
| mu5358-hw | S12 | vendor modules (thermal, DVFS, zram, Wi-Fi), zram, then the hotspot |
| mu5358-log | S13 | the system's state into the persistent log on the eMMC, every 5 s, after 5 min every minute |
| mu5358-modem | S15 | modem_control, cp_diskserver, refnotify in their chroot; NV only as RAM copies (`modem/README.md`) |
| mu300-atd | S19 | mu300's AT daemon (nr1 commands, nr2 data) |
| network | S20 | wan = mu300cell over sipa_eth0 (APN `internet`) |
| mu5358-buttons | S95 | power: short = screen on/off, held 3 s = power off; Wi-Fi key held 3 s = hotspot off/on |
| mu5358-cell | S99 | mu300's `mobile-data watch` (redial after two failed checks) |
| mu5358-usb | S99 | re-enumerates the USB gadget (5 s off) while no host has it: not configured, no carrier, or our TX still while the host's packets come in |

mu300-post is not used: it makes the running slot the default boot (`mu300-next-boot --rearm`).

## Hardware status (2026-10-30)

| | |
|---|---|
| eMMC, USB (NCM + ACM) | yes |
| Modem, mobile data | yes: Turk Telekom LTE; 14-19 Mbit/s at this place, Android 11-12 on the same spot |
| Wi-Fi | AIC8800D80X2: hotspot 5 GHz channel 36, HE80 (Wi-Fi 6), WPA2; the board's MAC and calibration |
| Screen | u30ui: papatya, status, Wi-Fi, SMS, battery, settings, clients, signal, actions, system, log; 5 MB RAM (no fbcon in GKI: DRM dumb buffers, PAGE_FLIP; the panel is mounted upside down and turns the picture itself, MADCTL through `tools/spispeed/panelcmd`: turned by the CPU it tore) |
| GPU | Mali-G57 (arch 9.0.9, DDK r49p1), OpenGL ES 3.2 / EGL 1.5 / Vulkan through Android's libGLES_mali.so in the chroot, on demand (`gpu/README.md`); no OpenCL in Android 15's driver. 60 s stress: 950 MHz, ~100 frames/s of a heavy shader, 40 °C, idle back at 384 MHz and powered off |
| Buttons | power, Wi-Fi key (volume keys are in the DT, not used) |
| Battery, charger | vendor charger-manager; at 100 % the battery idles (-5 mA) and the system runs on USB |
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
- Load average ~16 while idle: vendor kernel threads in D state (as on mu300, FINDINGS 31e).
- Boot-time WARNINGs that are harmless: device_links (ums9632_clk), sipa_eth0 "Incorrect netdev->dev_addr".

## Where the logs are

- The persistent log: sectors 122152960.. of mmcblk0 (1 MiB; boot/init's kernel log in the first half, mu5358-log's
  snapshots in the second). From Android: `dd if=/dev/block/mmcblk0 bs=512 skip=122152960 count=2048`.
- LK's log: the uboot_log partition (boot cause: `BootCause is ...`).
- A boot that never reaches userspace: sysdumpdb (LK minidump), `tools/sysdumpdb-extract.py`.
