# MU5358 Linux

OpenWrt on the **ZTE U30 Pro** (MU5358, Unisoc UMS9632) 5G pocket hotspot, next to Android, with the hotspot's own
screen as a light control panel. The sibling of [mu300-linux](https://github.com/dikeckaan/mu300-linux) (ZTE F50 /
MU300 and U30 Air, UMS9620), and built on its OpenWrt release.

Website: **https://kaandikec.com/mu5358-linux/** (English, Türkçe, 中文)

> **Status: a working research port, not a product.** A web installer exists (`site/web-installer`, `installer/`; the first release is v2026.10.30.7) and is tested over plain adb on one unit, not yet from the browser. Everything here
> was brought up and tested on one rooted unit. Linux only ever starts as a **one-shot trial from slot b**: the
> next reboot or power-off is Android again, by itself. Read the whole page before you try anything.

## What works (2026-10-30)

| | |
|---|---|
| Kernel | the board's own GKI 6.6.98 (android15-6.6), rebuilt with a small fragment (`kernel/`), with the stock vendor modules |
| Mobile data | Android's modem daemons in a chroot, as on mu300-linux; the NV partitions only ever as RAM copies (`modem/`). LTE, 14-19 Mbit/s where Android did 11-12 |
| Wi-Fi | AIC8800D80X2 hotspot, 5 GHz, HE80 (Wi-Fi 6), WPA2, with the board's own MAC and calibration |
| Screen | `u30ui2`, the dashboard: a status bar and eleven pages (live throughput graph, network with band/cell/signal history, Wi-Fi with a QR code to join, clients, traffic, SMS, battery, system, settings, power, log) in English, Turkish and Chinese; C on DRM, one 1.6 MB static binary, ~6 MB RAM, 0.5 % CPU (`tools/u30ui2/`). The launcher's look (`u30ui`, `tools/u30ui/`) stays as the classic interface |
| Buttons | power: screen on/off, held = power off; Wi-Fi key held = hotspot off/on |
| USB | network (NCM, `192.168.58.1`, SSH) and a serial console (ACM) |
| GPU | Mali-G57, OpenGL ES 3.2 / EGL 1.5 / Vulkan through Android's driver, on demand (`gpu/`) |
| Control panel | mu300-linux's LuCI panel (dashboard, cellular, SIM, SMS, traffic, TTL, terminal, VPN pages; English, Turkish, Chinese) under this project's name: the whole OpenWrt tree is mu300's openwrt-luci release renamed (`openwrt/rebrand.sh`); root's password is set by the installer |
| Stability | 3-hour soak clean (90/90 checks, no oops, NV untouched, flat memory); 6/6 reboot cycles clean; thermal-guard, lockup detectors with panic |

## What is missing

Suspend and power saving, charging verified below 100 %, battery-only (unplugged) operation tested over time,
Bluetooth, NFC, SIM tray detection, USB OTG host, calls / VoLTE / audio, sending SMS, OpenCL (Android 15's GPU
driver has none), an installer. A mainline kernel port is started and paused (`mainline/`).

## How it boots

1. Android (rooted, Magisk) writes a one-shot "try slot b" request into `misc`.
2. LK boots slot b: the rebuilt GKI kernel in `boot_b` with our `init_boot_b` ramdisk (`boot/`, `kernel/`).
3. The ramdisk loads the vendor modules, brings up USB, **puts `misc` back to Android at 1.6 s**, and switches into
   the OpenWrt root file system that lives on the eMMC behind `userdata`.
4. Any reboot, power-off, kernel oops (`panic_on_oops=1`) or failed boot lands in Android again.

Slot a and the NV partitions are never written.

## The repository

| | |
|---|---|
| `boot/` | the slot-b initramfs (`init`) and the script that builds `init_boot_b` |
| `kernel/` | the GKI rebuild: stock config, fragment, build container |
| `openwrt/` | `build-rootfs.sh`, `rebrand.sh` (mu300 → mu5358, the whole tree), `customize.sh`: the OpenWrt root file system from mu300-linux's openwrt-luci release, made for this board; `panel/po`: the panel's Turkish and Chinese |
| `modem/` | the modem chroot with NV as RAM copies |
| `gpu/` | Mali under Linux, `gltest` |
| `tools/u30ui2/` | the screen UI, the dashboard (preview PNGs in `tools/u30ui2/preview/`, in three languages) |
| `tools/u30ui/` | the classic screen UI, the launcher's look (`tools/u30ui/preview/`) |
| `installer/` | the device side of the web installer: `mu5358-install.sh`, the GPT and f2fs tools |
| `tools/screencon/` | the screen as a log console (fallback) |
| `tools/spispeed/` | two tiny kernel modules for the panel's SPI (the panel turns the picture: no tearing) |
| `tools/` | full backup, f2fs shrink, boot image and sysdump helpers |
| `mainline/` | the paused mainline port (patches, stub, trial script) |
| `docs/GKI-SYSTEM.md` | the system as it runs: services, hardware status, known behaviour, logs |

Releases are signed (`SHA256SUMS.sig`, the project's usign key `installer/release.pub`, number
`15fd2f702186c19e`): the web installer refuses a release without a valid signature. Releases are built by the
Release workflow (`.github/workflows/release.yml`, `tools/ci-release.sh`) on a GitHub arm64 runner from the sources
alone: the GKI kernel at the device's commit, the tools, the image on mu300-linux's rootfs, the signature with the
repository's secret key, then the GitHub release and the mirror branch `mirror/<tag>` the page downloads the files
from (GitHub's own release downloads cannot be read by a page; files the user picks remain the fallback). A new
release is a prerelease until promoted on GitHub; the page lists it as a test release and proposes the newest
promoted one. `tests/` holds the rules this
project keeps as tests (`python3 -m unittest discover -s tests`, `node --test tests/web/`), run by
`.github/workflows/tests.yml`.

Android's proprietary files (modem daemons, firmware, the GPU driver) are not here: the scripts pull them from your
own device. Device dumps and backups never belong in this repository.

## Safety rules this project keeps

- A **full backup** first (`tools/backup-full.sh`), checked against hashes taken on the device.
- **Never write the NV partitions**, not even from Linux: the modem gets loop devices over RAM copies.
- **Never write slot a**; Linux is never made the default boot.
- **A root password always**: the installer sets it (the page hashes it in the browser; only the hash reaches the
  device); the initramfs has no telnet, the system's SSH is the way in.
- **Never unbind the USB controller** on this vendor kernel (it oopses holding RTNL), and never read
  `/sys/kernel/debug/clk/clk_summary` (it hangs the kernel).

## Licence

Scripts, tools and documentation: MIT (`LICENSE`). Kernel modules and kernel patches: GPL-2.0, as marked in each
file. Roboto (in `tools/u30ui/third_party`): Apache-2.0; Noto Sans SC (`tools/u30ui2/third_party`): OFL-1.1. A hobby project, at your own risk, with no warranty. Not
endorsed by ZTE, Unisoc or any operator.
