# The modem under Linux (GKI OpenWrt)

Android's modem daemons run in a chroot at `/opt/mu5358/android`, as on mu300-linux, with one difference: **Linux
never writes the real NV**. The chroot's `/dev` is its own tmpfs with the character devices of `/dev` and no eMMC
node; every `/dev/block/by-name` entry in it is a loop device:

- the NV and the partitions next to it (prodnv, calinv, miscdata, persist, ztepersist, nr_factorynv,
  nr_runtimenv1/2, nr_downloadnv_a/b, nr_deltanv_a/b): writable loops over sparse copies in `/tmp/mu5358-nv`
  (117 MiB of RAM). What the modem writes to its NV is lost at reboot; Android stays the owner of the real NV.
- the images modem_control loads (nr_modem, nr_phy, nr_phy_npi, pm_sys, ch_sys, l_agdsp, both slots): read-only
  loops (`losetup -r`) over the real partitions.

The daemons get Android's slot (`_a`): `/proc/bootconfig` and `/proc/cmdline` in the chroot are copies with the
slot changed (Linux runs from b).

## Files

| | |
|---|---|
| `modem-start` | the chroot (`setup`), then `control` (modem_control), `diskserver`, `refnotify` |
| `mu5358-modem` | procd service, S15: the three daemons |
| `mu5358-cell` | procd service, S99: mu300's `mobile-data watch` (without mu300-post, which re-arms the boot slot) |
| `ueventd-perms.sh` | from this board's `vendor/etc/ueventd.rc` by mu300's `android-vendor/gen-ueventd-perms.py` |
| `patch-props.py` | the chroot's property area with `persist.log.tag` V instead of S |

`openwrt/customize.sh` installs them, turns mu300-atd (S19) back on and the wan on (`96-mu5358-wan`, APN `internet`
when none is set).

## The Android files (not in this repository)

From the running Android, on the Mac:

    ANDROID_SERIAL=<serial> sh ../mu300-linux/android-vendor/extract-subset.sh work/android-subset
    # four libraries Android 15 needs beyond mu300's list
    for f in /vendor/lib64/libnv_utils.so /vendor/lib64/librpmbclient.so /system/lib64/ld-android.so \
             /system/lib64/libapexsupport.so; do adb pull $f work/android-subset$f; done
    chmod u+w work/android-subset/dev/__properties__/*
    modem/patch-props.py work/android-subset/dev/__properties__

Then the tree goes to `/opt/mu5358/android` of the rootfs, owned by root, with `dev/__properties__` as
`dev-properties` (directories and programs 0755, the property files 0444, the rest 0644).

## What it took (2026-10-29)

- `persist.log.tag` is S on ZTE's Android: the daemons log nothing until the chroot's copy says V.
- modem_control switches to uid 1000 itself: `/dev/modem`, `pmsys`, `chsys`, `cp_dump` and `phycp` must be
  1000:1000 0660 (as on Android), else EACCES and no load.
- cp_diskserver must start after the CP is loaded (`modem modem@2: start over`): before, `/dev/modem` gives no NV
  layout and it loops on "get nv region failed" for good. On Android, init starts it when modem_control asks.
- All the modem modules, sipa-dele included, come from vendor_boot's list: loaded at boot as on Android.

Measured: the modem loaded 17.6 s into the boot, `+CPIN: READY`, registered on 286-03 (LTE), the wan up through
netifd's mu300cell at ~45 s, ping and NAT from a LAN client over the SIM; the 12 NV partitions identical before and
after a session (the RAM copy of nr_runtimenv was written).

## Wi-Fi (2026-10-29)

AIC8800D80X2 on SDIO: rfkill (this kernel's own module), the vendor's cfg80211, aic8800_bsp, aic8800_fdrv from
/lib/mu5358/modules (mu5358-hw), the firmware in /odm/firmware/aic8800 (on firmware_class.path). mu5358-hw then
makes the hotspot once with mu300's openwrt-wifi-config (U30Pro-XXXX, 5 GHz channel 36 HE80 (Wi-Fi 6) without DFS, country TR,
WPA2, random password in `uci get wireless.ap0.key`) and brings it up; wlan0 joins br-lan with usb0. Open: the
firmware gives no MAC (00:00:00:00:00:00) and the driver takes a random one every boot.
