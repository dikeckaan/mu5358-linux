#!/bin/sh
# The U30 Pro's changes to mu300-linux's OpenWrt root filesystem, on the tree at $1. build-rootfs.sh runs it on a
# fresh tree; it also runs from Android (mksh, toybox) on the installed system, mounted through a loop device:
#
#   sh customize.sh /data/local/tmp/mr
#
# so it only uses what both have, and running it twice changes nothing more.
set -e
R=${1:?usage: customize.sh ROOT}
[ -e "$R/etc/openwrt_release" ] || { echo "$R: not an OpenWrt tree" >&2; exit 1; }

# The tree is mu300-linux's openwrt-luci rootfs after rebrand.sh (every mu300 name is mu5358 here). Its services: the
# ones this board runs get their rc.d links (rebrand.sh took them all away: the rest drive F50 / U30 Air hardware or
# are not ported yet - power, cpu, modem-log, ndp, wifi-client, adbd).
for s in mu5358-accounts:11 mu5358-atd:19 mu5358-atd-dash:19 mu5358-smsd:95 mu5358-traffic:96 mu5358-toolkit:96 mu5358-user-packages:99; do
    n=${s%:*}; k=${s#*:}
    [ -f "$R/etc/init.d/$n" ] || { echo "$R: no service $n (not the openwrt-luci tree?)" >&2; exit 1; }
    ln -sf "../init.d/$n" "$R/etc/rc.d/S$k$n"
done
# fw4: the kernel has nftables (kernel/patches/0001-nftables-kmi.patch); before it, this service was off
[ -e "$R/etc/rc.d/S19firewall" ] || ln -sf ../init.d/firewall "$R/etc/rc.d/S19firewall"
# no TLS key in the image: uhttpd makes the device its own at the first start (mu300's FINDINGS 132: a shipped key
# is the same on every device)
rm -f "$R/etc/uhttpd.key" "$R/etc/uhttpd.crt"
# the project's release key (installer/release.pub), in place of mu300's: what a release's SHA256SUMS.sig must check
# against (tools/sign-release.sh, make-release.sh)
PUBKEY=$(cd "$(dirname "$0")/.." && pwd)/installer/release.pub
if [ -f "$PUBKEY" ]; then
    mkdir -p "$R/opt/mu5358/lib" && cp "$PUBKEY" "$R/opt/mu5358/lib/release.pub" && chmod 644 "$R/opt/mu5358/lib/release.pub"
else
    rm -f "$R/opt/mu5358/lib/release.pub"
fi
# mu300's adbd is not this board's (its USB hook waits for adbd's rebind unless adb is off)
mkdir -p "$R/etc/mu5358" && echo off > "$R/etc/mu5358/adb"
# thermal-guard (mu300's: caps the CPU when the kernel throttles, powers off at the critical temperature, a backstop
# over the vendor zones), from its own service here (mu300 ran it from mu300-post, which is not here)
# This board has 34 thermal zones, and some are not sensors: frame-thmzone read 86 C on a cool board and the guard
# throttled both clusters to 614 MHz (2026-10-30; at 105 C it would have powered off). hottest() takes only the
# SoC's, the clusters', the cores', the GPU's, the board's, the radios' and the battery's zones; and battery() reads
# this board's battery (it was the U30 Air's only).
f="$R/opt/mu5358/bin/thermal-guard"
if [ -f "$f" ] && ! grep -q 'mu5358: the zones that are sensors' "$f"; then
    grep -q '^        { read -r v < "$z"; } 2>/dev/null || continue$' "$f" || { echo "$f: no zone read line to patch" >&2; exit 1; }
    sed -i 's|^        { read -r v < "$z"; } 2>/dev/null \|\| continue$|        k=; { read -r k < "${z%/temp}/type"; } 2>/dev/null   # mu5358: the zones that are sensors\n        case $k in soc-thmzone\|cluster*-thmzone\|core?-thmzone\|gpu-thmzone\|board-thmzone\|lte-thmzone\|nr-thmzone\|chg-thmzone\|pa-thmzone\|pa5g-thmzone\|battery) ;; *) continue ;; esac\n&|' "$f"
    grep -q '^    local z v found=0$' "$f" && sed -i 's|^    local z v found=0$|    local z v k found=0|' "$f"
    grep -q '^    \[ "$DEVICE" = u30air \] \|\| return 0$' "$f" || { echo "$f: no u30air battery line to patch" >&2; exit 1; }
    sed -i 's|^    \[ "$DEVICE" = u30air \] \|\| return 0$|    case $DEVICE in u30air\|mu5358) ;; *) return 0 ;; esac|' "$f"
fi
cat > "$R/etc/init.d/mu5358-thermal" <<'EOF'
#!/bin/sh /etc/rc.common
# U30 Pro: thermal-guard (mu300's) as a service of its own
START=99
USE_PROCD=1

start_service() {
	procd_open_instance thermal
	procd_set_param command /opt/mu5358/bin/thermal-guard
	procd_set_param respawn 3600 5 0
	procd_close_instance
}
EOF
chmod 755 "$R/etc/init.d/mu5358-thermal"
ln -sf ../init.d/mu5358-thermal "$R/etc/rc.d/S99mu5358-thermal"

echo mu5358 > "$R/etc/mu5358-device"

# The LAN address: mu300-lan-ip gives 90-mu300 (a first install) and preinit's early DHCP server the device's default,
# 192.168.78.1 for anything it does not know (the U30 Air's). This board's is 192.168.58.1, boot/init's subnet:
# 192.168.79.0/24 and the mu300 ones can be on the same computer.
f="$R/opt/mu5358/bin/mu5358-lan-ip"
if [ -f "$f" ] && ! grep -q 'etc/mu5358-device' "$f"; then
    grep -q '^case ' "$f" || { echo "$f: no 'case' line to patch" >&2; exit 1; }
    sed -i '/^case /i\
[ -e /etc/mu5358-device ] \&\& { echo 192.168.58.1; exit 0; }' "$f"
fi

# After 90-mu300 (which sets the LAN address of a first install): the address and the name of this board (the wan,
# mu300cell from 90-mu300, is 96-mu5358-wan's below). The system section is made when there is none: the first boot
# on 2026-10-29 had only 90-mu300's marker there.
rm -f "$R/etc/uci-defaults/10-mu5358"
cat > "$R/etc/uci-defaults/95-mu5358" <<'EOF'
# U30 Pro (MU5358): the LAN on the USB gadget at 192.168.58.1, the name of the board
uci -q get system.@system[0] >/dev/null || uci -q add system system >/dev/null
uci -q batch <<'U'
set network.lan.ipaddr='192.168.58.1'
set network.lan.netmask='255.255.255.0'
set system.@system[0].hostname='U30Pro'
set system.@system[0].zonename='Europe/Istanbul'
set system.@system[0].timezone='<+03>-3'
commit network
commit system
U
exit 0
EOF

# Time from the network: mu300's image has no system.ntp, so sysntpd (S98) started nothing and the clock was only
# what Android had left in the RTC (2026-10-30). Made when there is none.
cat > "$R/etc/uci-defaults/97-mu5358-ntp" <<'EOF'
# U30 Pro: NTP on, OpenWrt's pool, when the system section has no ntp yet
if [ -z "$(uci -q get system.ntp)" ]; then
	uci -q batch <<'U'
set system.ntp=timeserver
set system.ntp.enabled='1'
set system.ntp.enable_server='0'
add_list system.ntp.server='0.openwrt.pool.ntp.org'
add_list system.ntp.server='1.openwrt.pool.ntp.org'
add_list system.ntp.server='2.openwrt.pool.ntp.org'
add_list system.ntp.server='3.openwrt.pool.ntp.org'
commit system
U
fi
exit 0
EOF

# The vendor modules Android loads from vendor_dlkm and boot/init does not (it loads vendor_boot's recovery list):
# thermal (the CPU DVFS has no cluster zones without sprd_virt_thm), DDR/SCU DVFS, zram, the boot reason. In Android's
# order (vendor_dlkm's modules.load), from /lib/mu5358/modules - not /lib/modules, where kmodloader would load
# whatever /etc/modules.d names (a panic on 2026-10-29). Each one was loaded on the running system first (2026-10-29).
# The .ko files come from the vendor_dlkm modules directory given as $2 (/vendor_dlkm/lib/modules under Android);
# without it the ones already in the tree stay.
MODS="sprd_cpu_cooling sprd_shell_thm sprd_virt_thm sprd_freq_limit sprd_ddr_dvfs dmc_drv sprd_bcl
sprd_ump96xx_tsensor thermal-generic-adc zsmalloc zram unisoc_pnp unisoc_scufreq cpufreq_userspace unisoc_last_kmsg
zte_bootreason cfg80211 aic8800_bsp aic8800_fdrv"
mkdir -p "$R/lib/mu5358/modules" "$R/etc/mu5358"
: > "$R/etc/mu5358/modules"
# Wi-Fi (AIC8800D80X2 on SDIO, 2026-10-29): the vendor's cfg80211 needs rfkill, which is this kernel's own module
# (system_dlkm's is the stock GKI's); its firmware where firmware_class.path looks, /odm/firmware/aic8800, from the
# directory given as $3 (/odm/firmware/aic8800 under Android)
if [ -f "$R/root/gki-modules-disabled/rfkill.ko" ]; then
    cp "$R/root/gki-modules-disabled/rfkill.ko" "$R/lib/mu5358/modules/"
fi
[ -f "$R/lib/mu5358/modules/rfkill.ko" ] && echo rfkill >> "$R/etc/mu5358/modules"
# userspace I2C (this kernel's i2c-dev, kernel/mu5358-linux.fragment): mu5358-charge programs the charger's input
# limit through /dev/i2c-6 (the vendor stack leaves it at 500 mA without Android's charging policy, 2026-11-01)
if [ -f "$R/root/gki-modules-disabled/i2c-dev.ko" ]; then
    cp "$R/root/gki-modules-disabled/i2c-dev.ko" "$R/lib/mu5358/modules/"
fi
[ -f "$R/lib/mu5358/modules/i2c-dev.ko" ] && echo i2c-dev >> "$R/etc/mu5358/modules"
FW=${3:-/odm/firmware/aic8800}
if [ -f "$FW/fmacfw_8800d80x2.bin" ]; then
    mkdir -p "$R/odm/firmware/aic8800"
    cp "$FW"/* "$R/odm/firmware/aic8800/"
fi
# and this board's own calibration from prodnv (Android's /mnt/vendor, before /odm/firmware/aic8800 on the firmware
# path): the TX power offsets and the crystal's load capacitance (xtal_cap; off in /odm's default file), and its Wi-Fi
# MAC (the firmware gives none, the driver a random one every boot). Read from Android's mount; Linux never mounts it.
PV=${4:-/mnt/vendor}
if [ -f "$PV/aic_userconfig_8800d80x2.txt" ]; then
    mkdir -p "$R/mnt/vendor"
    cp "$PV/aic_userconfig_8800d80x2.txt" "$R/mnt/vendor/"
fi
[ -f "$PV/wifimac.txt" ] && grep -q -E '^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$' "$PV/wifimac.txt" &&
    cp "$PV/wifimac.txt" "$R/etc/mu5358/wifimac"
for m in $MODS; do
    if [ -n "$2" ]; then
        [ "$2/$m.ko" -ef "$R/lib/mu5358/modules/$m.ko" ] || cp "$2/$m.ko" "$R/lib/mu5358/modules/" || { echo "$2/$m.ko missing" >&2; exit 1; }
    fi
    echo "$m" >> "$R/etc/mu5358/modules"
done
cat > "$R/etc/init.d/mu5358-hw" <<'EOF'
#!/bin/sh /etc/rc.common
# U30 Pro: the vendor modules of /etc/mu5358/modules (from /lib/mu5358/modules, in that order), then zram. In the
# background, as mu300-hw does: a module that hangs must not keep the consoles and the network from starting.
START=12

boot() {
	(
		for m in $(cat /etc/mu5358/modules); do
			grep -q "^$(echo $m | tr - _) " /proc/modules && continue
			/opt/mu5358/bin/busybox insmod /lib/mu5358/modules/$m.ko && echo "ok $m" || echo "FAILED $m"
		done
		/opt/mu5358/bin/zram-start && echo "zram on"
		# Wi-Fi: wlan0 appears after network (S20) started, so the hotspot comes up here. The first time, mu300's
		# openwrt-wifi-config makes it (5 GHz without DFS, country TR, WPA2 with a random password: uci
		# wireless.ap0.key) with this board's name.
		for i in $(seq 1 30); do [ -e /sys/class/net/wlan0 ] && break; sleep 1; done
		if [ -e /sys/class/net/wlan0 ]; then
			if [ ! -e /etc/mu5358/wifi-configured ] && [ ! -e /etc/mu5358/hotspot.conf ]; then
				mkdir -p /etc/mu5358
				{ echo "SSID=U30Pro-$(tr -d : < $( [ -s /etc/mu5358/wifimac ] && echo /etc/mu5358/wifimac || echo /sys/class/net/wlan0/address) | tr -d '\n' | tail -c 4 | tr a-f A-F)"
				  echo COUNTRY=TR; } > /etc/mu5358/hotspot.conf
			fi
			sh /opt/mu5358/bin/openwrt-wifi-config
			# Wi-Fi 6: the AIC8800 does HE in AP mode up to 80 MHz; mu300's script writes VHT (its SC2355 has no HE)
			case $(uci -q get wireless.radio0.htmode) in
				VHT*) uci set wireless.radio0.htmode="HE$(uci get wireless.radio0.htmode | tr -dc 0-9)" &&
					uci commit wireless ;;
			esac
			[ -s /etc/mu5358/wifimac ] && [ -z "$(uci -q get wireless.ap0.macaddr)" ] &&
				uci set wireless.ap0.macaddr="$(cat /etc/mu5358/wifimac)" && uci commit wireless
			iw reg set "$(uci -q get wireless.radio0.country || echo TR)"
			wifi up && echo "wifi up"
		fi
	) > /tmp/mu5358-hw.log 2>&1 &
}
EOF
chmod 755 "$R/etc/init.d/mu5358-hw"
ln -sf ../init.d/mu5358-hw "$R/etc/rc.d/S12mu5358-hw"

# An oops resets the board (into Android: Linux is a one-shot trial) 10 s later. It was off while the port was young;
# on 2026-10-30 an oops in rtnl_fill_ifinfo died holding the RTNL lock, and the board ran on with Wi-Fi and the
# screen up but every network command hung - a router that looks alive and is not. The trace stays in ramoops (a
# warm reset keeps the RAM) and in the persistent log's last snapshot.
# and the lockup detectors with a panic (mu300's FINDINGS 31d): a CPU stuck for 60 s, or a task stuck in D for 300 s,
# resets the board to Android instead of a router that looks alive and is not
printf 'kernel.panic_on_oops=1\nkernel.panic=10\nkernel.softlockup_panic=1\nkernel.hung_task_panic=1\nkernel.hung_task_timeout_secs=300\n' > "$R/etc/sysctl.d/90-mu5358.conf"

# The system's own log into the persistent log on the eMMC (boot/init's LOGSEC: kernel log in its first half, this
# in the second), read from Android after the next boot: the board has no serial console of its own.
cat > "$R/etc/init.d/mu5358-log" <<'EOF'
#!/bin/sh /etc/rc.common
# U30 Pro: the system - addresses, links, bridges, processes and logread - into the second 512 KiB of boot/init's
# persistent log (/etc/mu5358/persist-log, written by boot/init at every boot): every 5 s for the first five minutes, then every minute for as long as
# it runs (a USB loss on 2026-10-29 came after the five minutes; ~0.7 GB a day). Nowhere for any other root.
START=13

start() {
	# boot/init's: "DEVICE SECTOR", the persistent log's second half where this system lives
	read -r dev seek < /etc/mu5358/persist-log 2>/dev/null
	[ -b "$dev" ] && [ -n "$seek" ] || return 0
	(
		i=0
		while :; do
			{
				echo "MU5358-SYSLOG-BEGIN $(cut -d' ' -f1 /proc/uptime) $(date)"
				echo "usb $(cat /sys/class/udc/*/state 2>/dev/null) $(ip -o link show usb0 2>/dev/null | grep -o LOWER_UP)" \
					"battery $(cat /sys/class/power_supply/battery/capacity 2>/dev/null)%" \
					"$(cat /sys/class/power_supply/battery/current_now 2>/dev/null)uA" \
					"wan $(ifstatus wan 2>/dev/null | grep -c '"up": true') load $(cut -d' ' -f1-3 /proc/loadavg)" \
					"mem $(grep MemAvailable /proc/meminfo | tr -s ' ' | cut -d' ' -f2)kB"
				ip -o addr; ip -o link; ip route; ls /sys/class/net/*/brif 2>/dev/null
				ps w
				echo "--- logread"
				logread | tail -c 360000
				echo "MU5358-SYSLOG-END"
			} > /tmp/mu5358-log 2>&1
			head -c 8192 /dev/zero >> /tmp/mu5358-log
			dd if=/tmp/mu5358-log of=$dev bs=512 seek=$seek count=1024 conv=notrunc,fsync 2>/dev/null
			i=$((i + 1))
			[ $i -lt 60 ] && sleep 5 || sleep 60
		done
	) >/dev/null 2>&1 &
}
EOF
chmod 755 "$R/etc/init.d/mu5358-log"
ln -sf ../init.d/mu5358-log "$R/etc/rc.d/S13mu5358-log"
# The USB gadget again while no host has it (2026-10-29): one boot's first enumeration was missed (usb0 never got a
# carrier), mu300's hotplug hook re-enumerated once at 10 s, the Mac did not answer that either, and nothing tried
# again: the board ran on (Wi-Fi, wan) with no USB until a forced power-off. This one keeps trying, with growing gaps.
mkdir -p "$R/opt/mu5358/bin"
cat > "$R/opt/mu5358/bin/usb-watch" <<'EOF'
#!/bin/sh
# Re-enumerate the USB gadget (usb_gadget/linux) while no host has it: after 30 s, then 60, 120, 240 and every 300 s,
# back to 30 once a host has it. With no cable that is one pullup toggle every 5 min. A host has it when the UDC is
# configured and the NCM link has a carrier: the vendor musb kept saying "configured" after the D+ pullup was off
# (2026-10-29), so the state alone is not enough.
G=/sys/kernel/config/usb_gadget/linux
gap=30
sleep 30
while :; do
	udc=$(cat $G/UDC 2>/dev/null)
	st=$(cat /sys/class/udc/$udc/state 2>/dev/null)
	# usb0 by name: reading the NCM function's ifname takes the RTNL lock
	ifc=usb0
	# the carrier from the link flags: after a rebind of the controller usb0 lives on with its sysfs directory gone
	car=0; ip -o link show "${ifc:-usb0}" 2>/dev/null | grep -q LOWER_UP && car=1
	# a link whose sending side died (2026-10-30: the Mac's DHCP requests kept coming, usb0 sent nothing for minutes,
	# the Mac sat at 169.254): our TX still for 60 s while RX grew by 3 packets or more. Counters from /proc/net/dev,
	# which the rebind leaves working.
	set -- $(awk -v i="${ifc:-usb0}:" '$1 == i { print $3, $11 }' /proc/net/dev)
	rx=${1:-0}; tx=${2:-0}; t=$(cut -d. -f1 /proc/uptime); stalled=0
	if [ "$tx" != "${last_tx:-}" ]; then
		last_tx=$tx; ref_t=$t; ref_rx=$rx
	elif [ $((t - ref_t)) -ge 60 ] && [ $((rx - ref_rx)) -ge 3 ]; then
		stalled=1
	fi
	if [ -z "$udc" ] || { [ "$st" = configured ] && [ "$car" = 1 ] && [ $stalled = 0 ]; }; then
		gap=30
		sleep 10
		continue
	fi
	[ $stalled = 1 ] && st="$st, sending stalled"
	# a host on the port (a data port: SDP or CDP), else a charger or nothing, and nobody to enumerate with
	case $(cat /sys/class/power_supply/battery/usb_type 2>/dev/null) in
		*"[SDP]"*|*"[CDP]"*) ;;
		*) sleep 10; continue ;;
	esac
	# Only the gadget's own rebind: a rebind of the controller (musb-sprd unbind/bind, tried on 2026-10-30) leaves
	# usb0 with its parent device gone, and the next rtnl_fill_ifinfo oopsed on it holding the RTNL lock - every
	# network command hung from then on
	logger -t mu5358-usb "gadget $st (carrier ${car:-none}) on $udc: re-enumerating, next try in ${gap}s"
	# off for 5 s: long enough for a host's hub to see the device gone
	echo "" > $G/UDC 2>/dev/null
	sleep 5
	echo "$udc" > $G/UDC 2>/dev/null || logger -t mu5358-usb "could not bind $udc again"
	last_tx=
	sleep $gap
	[ $gap -ge 300 ] || gap=$((gap * 2))
	[ $gap -le 300 ] || gap=300
done
EOF
chmod 755 "$R/opt/mu5358/bin/usb-watch"
cat > "$R/etc/init.d/mu5358-usb" <<'EOF'
#!/bin/sh /etc/rc.common
# U30 Pro: the USB gadget re-enumerated while no host has configured it (/opt/mu5358/bin/usb-watch)
START=99
USE_PROCD=1

start_service() {
	procd_open_instance usbwatch
	procd_set_param command /opt/mu5358/bin/usb-watch
	procd_set_param respawn 3600 10 0
	procd_close_instance
}
EOF
chmod 755 "$R/etc/init.d/mu5358-usb"
ln -sf ../init.d/mu5358-usb "$R/etc/rc.d/S99mu5358-usb"

# The buttons (2026-10-30), from mu300's mu5358-keys (presses as text lines): the board has no LEDs (a screen, later),
# so mu300-buttons' LED logic does not fit. gpio-keys: power 116, volume 114/115, "Wifi Switch Key" 102.
cat > "$R/opt/mu5358/bin/buttons" <<'EOF'
#!/bin/sh
# U30 Pro buttons: power held 3 s shuts down cleanly, a short press switches the screen's backlight off or on; the
# Wi-Fi key held 3 s switches the hotspot off or on again. Other presses are logged only.
# One reader only: a restart by procd stops this shell, and the old mu5358-keys stays until its next write
for p in $(pidof mu5358-keys); do kill $p; done
# The reader runs in the background and this shell waits for it: a shell sits out a trap while a foreground
# pipeline runs, and procd stops a service by signalling this shell alone (mu300-linux's mu300-buttons, b3f199a8).
# A stop takes this shell's own children with it.
stop_children() {
	trap '' TERM INT HUP
	for s in /proc/[0-9]*/stat; do
		{ read -r p c st pp r < "$s"; } 2>/dev/null || continue
		[ "$pp" = "$$" ] && kill "$p" 2>/dev/null
	done
	return 0
}
trap 'stop_children; exit 143' TERM INT HUP
/opt/mu5358/bin/mu5358-keys -l 3000 | { while read -r code what; do
	case "$code $what" in
	"116 long")
		logger -t mu5358-buttons "power held: shutting down"
		poweroff ;;
	"102 long")
		if [ "$(uci -q get wireless.ap0.disabled)" = 1 ]; then
			uci set wireless.ap0.disabled=0; logger -t mu5358-buttons "Wi-Fi key held: hotspot on"
		else
			uci set wireless.ap0.disabled=1; logger -t mu5358-buttons "Wi-Fi key held: hotspot off"
		fi
		uci commit wireless
		wifi reload ;;
	"116 short")
		# bl_power 4 is off (the driver ignores the level then), 0 on
		b=/sys/class/backlight/sprd_backlight
		if [ "$(cat $b/bl_power)" = 0 ]; then echo 4 > $b/bl_power; else echo 0 > $b/bl_power; echo 2048 > $b/brightness; fi ;;
	"0 tick") ;;
	*) logger -t mu5358-buttons "key $code $what" ;;
	esac
done; } &
wait $!
EOF
chmod 755 "$R/opt/mu5358/bin/buttons"
cat > "$R/etc/init.d/mu5358-buttons" <<'EOF'
#!/bin/sh /etc/rc.common
# U30 Pro: the buttons (/opt/mu5358/bin/buttons)
START=95
USE_PROCD=1

start_service() {
	[ -x /opt/mu5358/bin/mu5358-keys ] || return 0
	procd_open_instance buttons
	procd_set_param command /opt/mu5358/bin/buttons
	procd_set_param respawn 3600 5 0
	procd_close_instance
}
EOF
chmod 755 "$R/etc/init.d/mu5358-buttons"
ln -sf ../init.d/mu5358-buttons "$R/etc/rc.d/S95mu5358-buttons"

# The screen as a console for now (2026-10-30): tools/screencon draws the kernel's and the system's log on the 240x240
# SPI panel through DRM (the GKI kernel has no fbcon). The periodic noise (charger, fuel gauge, power debug, eMMC
# speed, SFP, AIC tx power) stays off it; a short press of the power key switches the backlight off or on.
M2=$(cd "$(dirname "$0")/.." && pwd)/tools/screencon
M3=$(cd "$(dirname "$0")/.." && pwd)/tools/u30ui
if [ -f "$M3/u30ui" ]; then
    cp "$M3/u30ui" "$R/opt/mu5358/bin/u30ui"
    chmod 755 "$R/opt/mu5358/bin/u30ui"
fi
# the dashboard (tools/u30ui2: English, Turkish, Chinese), the default screen; /etc/mu5358/ui = classic picks the
# launcher-look u30ui (either one's settings page switches)
M5=$(cd "$(dirname "$0")/.." && pwd)/tools/u30ui2
if [ -f "$M5/u30ui2" ]; then
    cp "$M5/u30ui2" "$R/opt/mu5358/bin/u30ui2"
    chmod 755 "$R/opt/mu5358/bin/u30ui2"
fi
[ -n "${VERSION:-}" ] && echo "$VERSION" > "$R/etc/mu5358/version"
# u30ui turns the picture with the panel's MADCTL through this (tools/spispeed; without it the CPU turns it, and
# the frames tear)
M4=$(cd "$(dirname "$0")/.." && pwd)/tools/spispeed
[ -f "$M4/panelcmd.ko" ] && cp "$M4/panelcmd.ko" "$R/lib/mu5358/modules/"
# The GPU on demand (gpu/README.md): mali_kbase is loaded by gpu-run, not at boot; its userspace (libGLES_mali.so)
# is in the modem chroot when gpu/pull-gpu.sh brought it into work/android-subset
G=$(cd "$(dirname "$0")/.." && pwd)/gpu
[ -n "$2" ] && [ -f "$2/mali_kbase.ko" ] && cp "$2/mali_kbase.ko" "$R/lib/mu5358/modules/"
cp "$G/gpu-run" "$R/opt/mu5358/bin/gpu-run"
chmod 755 "$R/opt/mu5358/bin/gpu-run"
if [ -f "$G/gltest" ]; then
    mkdir -p "$R/opt/mu5358/android/opt"
    cp "$G/gltest" "$R/opt/mu5358/android/opt/gltest"
    chmod 755 "$R/opt/mu5358/android/opt/gltest"
fi
if [ -f "$M2/screencon" ]; then
    cp "$M2/screencon" "$R/opt/mu5358/bin/screencon"
    chmod 755 "$R/opt/mu5358/bin/screencon"
    cat > "$R/opt/mu5358/bin/screen-console" <<'EOF'
#!/bin/sh
# The console on the screen: the kernel log (from the start) and the system log, without the periodic noise.
# One screencon only (it is DRM master): a restart by procd stops this shell and leaves the old one up.
for p in $(pidof screencon); do kill $p; done
# The reader runs in the background and this shell waits for it: a shell sits out a trap while a foreground
# pipeline runs, and procd stops a service by signalling this shell alone (mu300-linux's mu300-buttons, b3f199a8).
# A stop takes this shell's own children with it.
stop_children() {
    trap '' TERM INT HUP
    for s in /proc/[0-9]*/stat; do
        { read -r p c st pp r < "$s"; } 2>/dev/null || continue
        [ "$pp" = "$$" ] && kill "$p" 2>/dev/null
    done
    return 0
}
trap 'stop_children; exit 143' TERM INT HUP
{ dmesg -w & logread -f; } 2>/dev/null | awk '
	/charger-manager|charger_policy|policy: |sprd-fgu|sc27xx-fgu|SPRD_PDBG|\|__|mmc0: r_speed|SFP:|AICWFDBG|thmzone|bq2560x|zte_power_supply|sprd_iommu|cm_batt_works|typec-pd/ { next }
	{ sub(/^\[ *[0-9]+\.[0-9]+\]\[[^]]*\] /, "["); print; fflush() }' | /opt/mu5358/bin/screencon &
wait $!
EOF
    chmod 755 "$R/opt/mu5358/bin/screen-console"
    cat > "$R/etc/init.d/mu5358-screen" <<'EOF'
#!/bin/sh /etc/rc.common
# U30 Pro: the screen - the dashboard (tools/u30ui2), the launcher's papatya (tools/u30ui) when /etc/mu5358/ui says
# classic, else the log as a console
# (/opt/mu5358/bin/screen-console)
START=11
USE_PROCD=1

start_service() {
	# DRM master is one client's: a screencon left by the console's pipe would keep u30ui off the panel
	for p in $(pidof screencon); do kill $p; done
	procd_open_instance screen
	ui=$(cat /etc/mu5358/ui 2>/dev/null)
	if [ "$ui" != classic ] && [ -x /opt/mu5358/bin/u30ui2 ]; then
		procd_set_param command /opt/mu5358/bin/u30ui2
	elif [ -x /opt/mu5358/bin/u30ui ]; then
		procd_set_param command /opt/mu5358/bin/u30ui
	else
		procd_set_param command /opt/mu5358/bin/screen-console
	fi
	procd_set_param respawn 3600 5 0
	procd_set_param stderr 1
	procd_close_instance
}
EOF
    chmod 755 "$R/etc/init.d/mu5358-screen"
    ln -sf ../init.d/mu5358-screen "$R/etc/rc.d/S11mu5358-screen"
fi

# mu300's LAN hook re-enumerates the gadget at every LAN ifup, for a macOS that kept the link "inactive". Here it
# broke a working one (2026-10-30: usb0 had its carrier at 8.3 s, the rebind at 10.4 s, and this Mac never came back
# until the cable was pulled): no rebind while usb0 has a carrier.
f="$R/etc/hotplug.d/iface/10-mu5358-usb"
if [ -f "$f" ] && ! grep -q LOWER_UP "$f"; then
    grep -q '^stamp=/tmp/mu5358-usb-rebound$' "$f" || { echo "$f: no stamp line to patch" >&2; exit 1; }
    sed -i 's|^stamp=/tmp/mu5358-usb-rebound$|# the U30 Pro: not while the host has the link (carrier)\nip -o link show usb0 2>/dev/null \| grep -q LOWER_UP \&\& exit 0\n&|' "$f"
fi

# The modem (2026-10-29): Android's daemons in a chroot whose NV is RAM copies (modem/modem-start), mu300's AT daemon
# and its mobile data watchdog, the wan on. The chroot's Android files are not in this repository: modem/README.md
# says how they are put in /opt/mu5358/android. The modem files come from modem/ next to this script's directory.
M=$(cd "$(dirname "$0")/.." && pwd)/modem
if [ -f "$M/modem-start" ]; then
    mkdir -p "$R/opt/mu5358/bin"
    cp "$M/modem-start" "$M/ueventd-perms.sh" "$R/opt/mu5358/bin/"
    cp "$M/mu5358-modem" "$M/mu5358-cell" "$R/etc/init.d/"
    chmod 755 "$R/opt/mu5358/bin/modem-start" "$R/etc/init.d/mu5358-modem" "$R/etc/init.d/mu5358-cell"
    ln -sf ../init.d/mu5358-modem "$R/etc/rc.d/S15mu5358-modem"
    ln -sf ../init.d/mu5358-cell "$R/etc/rc.d/S99mu5358-cell"
    cat > "$R/etc/uci-defaults/96-mu5358-wan" <<'EOF'
# U30 Pro: the wan on, now that the modem is ported (95-mu5358 had it off); the APN of the SIM it was brought up
# with (Turk Telekom) when none is set
uci -q set network.wan.auto='1'
[ -n "$(uci -q get network.wan.apn)" ] || uci -q set network.wan.apn='internet'
uci -q commit network
exit 0
EOF
fi

# The control panel (luci-app-mu5358, mu300's for the F50 / U30 Air / V50) on this board (2026-10-30). The pages that
# cannot apply go out of the menu: Dual SIM (one slot here; SIM settings stays), LEDs (none), Update (mu5358-update
# is rebrand.sh's refusing stub: updates are done from Android with the web installer). The views and the ACLs stay;
# an entry is the block from its key line to the next "}," line.
f="$R/usr/share/luci/menu.d/luci-app-mu5358.json"
[ -f "$f" ] || { echo "$f: no panel menu (not the openwrt-luci tree?)" >&2; exit 1; }
for k in admin/modem/sim admin/system/mu5358-leds admin/system/mu5358-update; do
    grep -q "^[[:space:]]*\"$k\": {\$" "$f" || continue
    sed -i "\\@^[[:space:]]*\"$k\": {\$@,\\@^[[:space:]]*},\$@d" "$f"
    ! grep -q "\"$k\"" "$f" || { echo "$f: $k is still in the menu" >&2; exit 1; }
done
# The dashboard's boot card: Linux is a one-time trial from slot b and nothing in it can change the boot, so there is
# no dual boot to switch or lock (no "dualboot" capability: the two buttons go; mu5358-next-boot refuses anyway), the
# boot adapter answers this board's state (the default boot Android, this boot one-time, the control not writable)
# with "trial", and the card says so in a note of its own (home.js, the string in openwrt/panel/po).
f="$R/usr/libexec/unisoc-modem/dashboard-info"
if [ -f "$f" ] && ! grep -q 'mu5358: no dual boot' "$f"; then
    grep -q '^CAP_DUALBOOT=0; \[ -x /opt/mu5358/bin/mu5358-next-boot \] && CAP_DUALBOOT=1$' "$f" || { echo "$f: no CAP_DUALBOOT line to patch" >&2; exit 1; }
    sed -i 's|^CAP_DUALBOOT=0; \[ -x /opt/mu5358/bin/mu5358-next-boot \] && CAP_DUALBOOT=1$|&\n[ -e /etc/mu5358-device ] \&\& CAP_DUALBOOT=0   # mu5358: no dual boot, Linux is a one-time trial (customize.sh)|' "$f"
fi
f="$R/usr/libexec/unisoc-modem/boot"
if [ -f "$f" ] && ! grep -q 'etc/mu5358-device' "$f"; then
    grep -q '^\[ -x "\$NB" \] || {' "$f" || { echo "$f: no NB line to patch" >&2; exit 1; }
    sed -i 's@^\[ -x "\$NB" \] || {@# mu5358 (U30 Pro): a one-time trial boot from slot b, the default boot Android, nothing here can change it (customize.sh)\n[ -e /etc/mu5358-device ] \&\& { echo '\''{"ok":1,"available":1,"default":"android","temporary":true,"locked":0,"attempts":null,"writable":0,"trial":1}'\''; exit 0; }\n&@' "$f"
fi
f="$R/www/luci-static/resources/view/mu5358/home.js"
if [ -f "$f" ] && ! grep -q 'mud-boot-trial' "$f"; then
    grep -q '^  <div class="mud-rows" id="mud-boot" style="display:none;margin-top:10px">$' "$f" || { echo "$f: no boot card line to patch" >&2; exit 1; }
    sed -i 's|^  <div class="mud-rows" id="mud-boot" style="display:none;margin-top:10px">$|&\n    <div class="mud-note" id="mud-boot-trial" style="display:none">${_("Linux runs as a one-time trial on this device: every restart or power-off returns to Android, and nothing here can change that. The next trial is started from Android again.")}</div>|' "$f"
    grep -q '^[[:space:]]*return L.resolveDefault(M.callBootGet(), {}).then(function(b) {$' "$f" || { echo "$f: no loadBoot line to patch" >&2; exit 1; }
    sed -i 's|^\([[:space:]]*\)return L.resolveDefault(M.callBootGet(), {}).then(function(b) {$|&\n\1var trial = root.querySelector("#mud-boot-trial"); if (trial) trial.style.display = b \&\& b.trial ? "" : "none";   /* mu5358: the one-time trial note (customize.sh) */|' "$f"
fi
# System > CPU: the CPU voltage offset is the F50 firmware's (mu300's FINDINGS 133); not drawn here, and not asked for.
# The profiles, the clusters, the cores and the thermal zones are generic and stay.
f="$R/www/luci-static/resources/view/mu5358/cpu.js"
if [ -f "$f" ] && ! grep -q 'mu5358: no CPU voltage' "$f"; then
    grep -q '^[[:space:]]*this.paintVoltage((res && res\[1\]) || {});$' "$f" || { echo "$f: no paintVoltage line to patch" >&2; exit 1; }
    sed -i 's@^\([[:space:]]*\)this.paintVoltage((res && res\[1\]) || {});$@\1/* mu5358: no CPU voltage offset on this board (the F50 firmware has it): the card is not drawn (customize.sh) */@' "$f"
    grep -q ', L.resolveDefault(M.callCpuVoltageGet(), {}) \]);$' "$f" || { echo "$f: no callCpuVoltageGet line to patch" >&2; exit 1; }
    sed -i 's@, L.resolveDefault(M.callCpuVoltageGet(), {}) \]);$@ ]);@' "$f"
fi
# Cellular > Device management: the USB role (host mode: the F50's dwc3 role switch), the USB network protocol for
# the next boot (mu300's boot/init reads /etc/mu5358/usb-net; this board's does not) and the USB adapters (host mode
# only) are not this board's, whose MUSB is a gadget in peripheral mode only: those cards are hidden, a note in their
# place (the string in openwrt/panel/po). The TCP congestion control and the operations log are generic and stay.
f="$R/www/luci-static/resources/view/mu5358/device.js"
if [ -f "$f" ] && ! grep -q 'mu5358: USB' "$f"; then
    grep -q '^[[:space:]]*this.root = root;$' "$f" || { echo "$f: no this.root line to patch" >&2; exit 1; }
    sed -i 's@^\([[:space:]]*\)this.root = root;$@&\n\1/* mu5358: USB is the MUSB gadget in peripheral mode only, and boot/init reads no protocol choice: the role, the network mode and the adapter cards are hidden, a note in their place (customize.sh) */\n\1root.querySelector(".mud-device-grid").style.display = "none";\n\1root.querySelector("#mud-usb-adapters-card").style.display = "none";\n\1root.insertBefore(E("section", { "class": "mud-card" }, [ E("h3", {}, "USB"), E("div", { "class": "mud-note" }, [ _("USB on this device is the network link to the computer (NCM, device mode only): there is no host mode and no other protocol to choose.") ]) ]), root.querySelector(".mud-device-grid"));@' "$f"
fi
# The hotspot watched (openwrt/wifiwatch, after mu300's wifiretry): three misses of hostapd or wlan0 in a row bring
# the radio back with wifi up, at most once in 10 minutes
WW=$(cd "$(dirname "$0")" && pwd)/wifiwatch
if [ -f "$WW" ]; then
    cp "$WW" "$R/opt/mu5358/bin/wifiwatch" && chmod 755 "$R/opt/mu5358/bin/wifiwatch"
    cat > "$R/etc/init.d/mu5358-wifiwatch" <<'EOF'
#!/bin/sh /etc/rc.common
# U30 Pro: the hotspot watched (/opt/mu5358/bin/wifiwatch)
START=99
USE_PROCD=1

start_service() {
	procd_open_instance wifiwatch
	procd_set_param command /opt/mu5358/bin/wifiwatch
	procd_set_param respawn 3600 5 0
	procd_close_instance
}
EOF
    chmod 755 "$R/etc/init.d/mu5358-wifiwatch"
    ln -sf ../init.d/mu5358-wifiwatch "$R/etc/rc.d/S99mu5358-wifiwatch"
fi
echo "customized $R"
