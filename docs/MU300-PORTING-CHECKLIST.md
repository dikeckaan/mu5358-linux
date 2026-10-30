# Porting checklist from mu300-linux

What [mu300-linux](https://github.com/dikeckaan/mu300-linux) already solved, distilled from its FINDINGS and scripts on 2026-10-29, as a map for the U30 Pro port. Paths are relative to mu300-linux; RB = rootfs/overlay/opt/mu300/bin, OI = openwrt/overlay/etc/init.d.

## 1. Modem bring-up

**Order of services (OpenWrt):**
- `OI/mu300-vendor` starts at START=09. Keep the leading zero: "S9" sorts after S19 and delays the CP by about 31 s. It starts:
  - `cp_diskserver` first. It is the NV server. Without it the modem stalls with "wait modem alive timeout" (§18, §13d).
  - Then `RB/android-vendor-start`, which execs `modem_control`.
  - Then `refnotify`, after `sleep 12`.
  - All three run through `RB/android-run`, with procd respawn.
- `OI/mu300-atd` starts at START=19 and holds two daemons:
  - nr1 is the general command channel and also drains nr0 (URCs).
  - nr2 belongs to mobile-data only.
  - It also runs a `radio-warmup` instance (`mobile-data radio-on`), skipped in a charging boot.
- netifd's `mu300cell` proto dials, then `OI/mu300-post` (S99) runs the `mobile-data watch` watchdog.

**What android-vendor-start does, in order:**
1. Fixes owners and modes of the imported files (root, 0755/0644/0444).
2. Waits up to 90 s for `/dev/modem` or `/dev/pmsys`.
3. Starts `logdw` on `/dev/socket/logdw` (liblog sink, `tools/logdw`).
4. Bind-mounts `$A/dev-properties` onto `/dev/__properties__`. It binds rather than copies: OpenWrt's /dev is a 512 KiB tmpfs and a copy silently ends up as empty files.
5. Creates `/dev/block/by-name/*` from PARTNAME in each partition's uevent, on the disk whose type is MMC (§31l).
6. Runs `ueventd-perms.sh`.
7. Mounts rbind /dev, proc and sysfs into the chroot.
8. Writes `/run/mu300-cmdline.android` with `androidboot.slot_suffix` swapped to Android's slot, and bind-mounts it over the chroot's `/proc/cmdline` (§5.3). It falls back to `/proc/device-tree/chosen/bootargs` when the cmdline has no slot.
9. Execs `modem_control` directly: `sprd_modem_loader` checks that the task name (comm) is exactly `modem_control` (§5.4).

**Chroot contents:** `android-vendor/subset-files.txt` and `extract-subset.sh`:
- linker64 and bionic
- about 22 system libs, including libtrusty and libbinder
- `vendor/lib64/libkernelbootcp.trusty.so`, `lib_crypto.so`
- `modem_control`, `cp_diskserver`, `refnotify`
- `vendor/etc/modem_{cp,sp,ch}_info.xml`, `cp_dump_info.xml`, `ueventd.rc`
- `/dev/__properties__`
- `linkerconfig/ld.config.txt` is created empty.

`android-run` unsets LD_PRELOAD, sets LD_LIBRARY_PATH to the bionic, /system/lib64 and /vendor/lib64 paths, and chroots. On OpenWrt the daemons switch to uid 1000 themselves, so the device nodes must belong to uid 1000 (§41a, issue #314). `pull_closure.py` pulls any other binary's library closure; its search list hard-codes `com.android.vndk.v33`, which is Android 13 and needs adjusting for 15.

**ueventd perms:** `android-vendor/gen-ueventd-perms.py` generates `ueventd-perms.sh` from the vendor `ueventd.rc`. Regenerate it from the U30 Pro's own ueventd.rc. Example entries: `by-name/nr_*`, `pm_sys*`, `calinv`, `stime_pm`.

**Traps:**
- **§5:** The PM co-processor watchdog cuts power at about 290 s unless `modem_control` reloads pm_sys through Trusty. Only after that is the sbuf channel ready for `sprd_pmic_wdt` to send `watchdog rstoff`. Success shows as "sbuf ready for pmic wdt init!".
- **§13b:** Exactly one reader per stty. Never close nr1 while the modem runs: the channel never answers again until reboot. Reopen only after 5 unanswered commands, with nr0 quiet for 240 s, at most once per 5 min.
  - Ownership lock is `/run/mu300-at/owner`; `lock` is the per-client mutex (§26d).
  - No `stty -F` on these ttys, and no `trap … EXIT` cleanup in ash (it fires when every subshell exits).
- **§13c:** Use `read -t` from bash or busybox ash; dash has none. The termios timer (`stty min 0 time 5`) is ignored. Apply stty to the already-open fd.
- **§13d and §13e:** These were mainline driver bugs (write-back mapping of shared memory, the mailbox fifo). With stock vendor modules they are probably not your problem, but the symptoms are worth recognising: "fail SIZE" with "wait modem alive timeout" (§13d); the AP stops sending at about 90 s (§13e).
- **§13f:** `sipa-dele.ko` must load after the CP answers and before `AT+CGACT`. Too early and its conn_thread exits for good (the module is [permanent]). Too late and the probe CFI-panics.
  - `RB/sipa-dele-start` waits for `+CGATT: 1`. Its only caller is mobile-data, between registration and the PDP context.
  - §31f: with the WAN already up, the vendor `sipa_dele` treats -EINPROGRESS as a failure, which crashes the board. The fix was a patch (`kernel/patches/sipa-delegate-einprogress.patch`), so your unmodified stock module likely still has this bug: keep the load order strict.
- **§13g:** A dead downlink can be a dropped cid 1 context (`+CGACT:1,0`) while `sipa_eth0` keeps its old address. Idle SIPA reads as broken (suspend_stage 0x3f), so drive traffic before reading the counters.
- **§26 and §31c:** The modem resets itself and comes back with `+CFUN: 0` and no context. The watchdog polls `AT+CGACT?` plus `AT+CGPADDR` against the interface address every 30 s and re-dials after two failures.
- **§196:** In a charger boot (`androidboot.mode=charger … modem=shutdown`), stock modem_control loads no CP. `RB/mu300-modem-coldboot` works around it.

**Dial (`RB/mobile-data` up_locked; `openwrt/overlay/lib/netifd/proto/mu300cell.sh`):**
1. Radio on (§15): the radio is at `+CFUN: 0` after boot. Send `AT+SFUN=4` and wait for `+CFUN: 1`; last resort is `AT+SFUN=2` then `AT+SFUN=4`.
2. Wait for registration (`AT+CEREG`/`AT+COPS`).
3. `AT+CGDCONT=1,"IPV4V6","apn",…`.
4. Run sipa-dele-start.
5. `AT+CGACT=1,1`.
6. `AT+CGCONTRDP=1` gives the address and DNS.
7. `AT+CGDATA="M-ETHER",1`, wait up to 45 s for CONNECT.
8. Configure `sipa_eth<cid-1 + slot*8>`: raw IP, address /32, `default dev`.

The proto parses IP=, IFACE=, PREFIX= and DNS1= from the output. fw4 needs `openwrt/patches/fw4-sipa-offload.patch` because sipa_eth0 is l3_device only.

**F50-specific vs generic:** The SFUN/CGDATA M-ETHER/sipa/stty_nr scheme is generic Unisoc. Specific to the board or vendor: the slot-to-nr mapping for SIM2 (nr4/nr5 to sipa_eth8), `mu300-sim`, the V50 SIM switches, and the coldboot workaround (U30 Air).

## 2. Wi-Fi

**Driver-independent, reusable:**
- `RB/openwrt-wifi-config`: one-time `/etc/config/wireless` from `/etc/mu300/hotspot.conf`; never a DFS channel; `ap0` → `lan`, psk2. It needs `opt/mu300/lib/wifi-channels.sh` and assumes phy0/wlan0.
- `RB/regdb-load` (§22): feeds the pending sysfs firmware requests, then `iw reg reload`. The `wens` certificate patch was for 5.4; check your GKI's certificates.
- `OI/mu300-hw` ordering: regdb, then `iw reg set`, then wifi-start, then the config, then `wifi down/up` once the country is live.
- One `br-lan` (§21).
- `openwrt/patches/wifi-scripts-no-dfs.patch` and `wifi-scripts-wpa2-only.patch`, `RB/wifi-ready`.

**SC2355-only, rewrite for AIC8800:**
- `RB/wifi-start`: pcie-sprd, wcn_bsp, sprd_wlan_combo, `/proc/mdbg`.
- §14 firmware ini; §21 IFF_DONT_BRIDGE patch; §23 DS-param patch and the one-AP limit.
- §14b one-way station mode: re-measure on AIC8800.
- The `FW_ERROR unisocwl` hotplug `openwrt/overlay/etc/hotplug.d/misc/10-mu300-wcn` and `RB/wcn-reopen`.
- The `wifiretry` in mu300-post (EIO on the first IFF_UP).
- §45 rmmod panic.

## 3. Stability and diagnostics

- **Logs (§3, §20):** pstore survives only a warm reset. `RB/early-recorder` writes dmesg into the Linux slot's boot partition at 48 MiB (8 MiB area): every 2 s for 90 s, then every 10 s, up to 420 s. It is started from `openwrt/overlay/lib/preinit/05_mu300_early_recorder`.
- **LK log (§1):** the `uboot_log` partition holds `rst_mode`, "charge first poweron reset" and the watchdog flags.
- **`tools/collect-logs.sh`:** run from Android; pulls pstore, uboot_log and the persist area.
- **§20b:** `console-ramoops-0` ends with "reboot: Restarting system with command …" for a userspace reboot, and mid-line for a power cut or watchdog.
  - On OpenWrt the reboot is always logged as [T1] (procd), so to find who asked: a kprobe on `__arm64_sys_reboot` piped from `trace_pipe` to disk, plus a log line in `/etc/rc.button/*`.
  - Never wrap `/sbin/reboot`: it deadlocks.
- **§31d:** Enable the soft and hard lockup detectors with panic, use `log_buf_len=4M`, and `systemd-pstore` moves records out of `/sys/fs/pstore`.
  - The PMIC watchdog's maximum is 300 s (UMP9620): watch this on yours.
- **Boot success:** `OI/mu300-post` runs `sleep 30; mu300-next-boot --rearm` (`RB/mu300-next-boot`, §17, §1).
  - LK treats tries==1 with successful==0 as already failed, so a one-shot trial needs tries=2.
- **`RB/bootmark`:** appends `t=<uptime> words` to `/run/mu300/boot-timeline`.
- **Unexplained cold resets:** apart from §5 (the PM watchdog), the only documented hangs are §31m (mainline, Mali probe). Nothing else found.

## 4. Power, LEDs, buttons

- **`RB/mu300-led`:** writes `/sys/class/leds/<name>/brightness` by device table.
  - F50: `sc27xx:{red,green,blue}` and `keyboard-backlight`.
  - U30 Air: also `zte-ldo0..2` (mainline) or `zte_ldo_leds` `vddcama*_status` (5.4), and `net_blue`.
  - All writes are best effort: the PMIC sometimes rejects one (§13g).
- **`RB/mu300-buttons`** with `tools/keys/mu300-keys.c`:
  - Reads every `/dev/input/event*` that has keys. KEY_POWER 116: short press wakes the LEDs, held 3 s shuts down. KEY_WIFI 138 is on the U30 Air.
  - `OI/mu300-buttons` exits silently if `mu300-keys` is missing.
  - §20b: `gpio-keys` has no KEY_RESTART.
- **`RB/mu300-power` (§36):** reads `/sys/class/power_supply/*` (type, present, capacity, temp, current_now), `/sys/class/extcon/*/state` (USB=1) and `/sys/class/udc`. The charge guard writes the bq256xx `charge_type` (N/A or Fast). Charging-boot marker is `/run/mu300/charging-boot`. Idle uses `AT+SPENDC=2` and `AT+SFUN=5`.

## 5. Other musts

- **§4:** The UDC appears only once the whole chain probes: extcon-usb-gpio, PHYs, `sc27xx_adc`, battery_info, charger-manager, fgu, `bq2560x` (otg-vbus regulator), dwc3/musb. `sc27xx_adc` must load before the fuel gauge, which errors hard and is never probed again. The order is in `boot/module-order.txt`.
- **§12:** Bring usb0 up immediately after binding the UDC. `mu300-usb-reset --if-no-lease 25` in mu300-post re-enumerates if no host took a lease.
- **§31h:**
  - `regulator_ignore_unused` is required: `LDO_VDDSIM0` powers the SIM and no driver claims it.
  - `fw_devlink=permissive` is required for the USB gadget.
  - `clk_ignore_unused`: 315 of 551 clocks have no driver holding them.
  - Power-off is the PMIC `PWR_PD_HW` write; check the stock sc27xx-poweroff on 6.6.
- **§27:** About 464 MiB reserved for the CP; zram is lz4 at half the RAM.
- **§18:** Load `sprd_cpu_cooling`, `leds-sc27xx-bltc` etc. after the modem.
- **§31e, §6:** D-state vendor threads inflate the load average.
- **§41a:** OpenWrt has no setpriv `--reuid`, so the nodes must be uid 1000's.
- **§38c:** fw4 flowtable on an old kernel (`fw4-old-kernel-flowtable.patch`, needed only before 5.13).
