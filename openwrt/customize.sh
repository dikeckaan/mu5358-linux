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

# mu300's hardware services and early hooks off: they drive F50/U30 Air hardware (LEDs, buttons, modem, Wi-Fi, CPU,
# power, adbd, vendor chroot) that this board does not have or has elsewhere; they come back one at a time as they
# are ported. mu300-accounts stays (root's password and the web login).
for s in "$R"/etc/init.d/mu300-*; do
    n=${s##*/}
    [ "$n" = mu300-accounts ] && continue
    rm -f "$R"/etc/rc.d/S??"$n" "$R"/etc/rc.d/K??"$n"
done
rm -f "$R/lib/preinit/04_mu300_early_leds"
# mu300's early recorder writes its log 48 MiB into the boot partition of the slot Linux runs from: here boot_b, our
# kernel's (behind the part AVB signs, but written on every boot; 2026-10-29). mu5358-log below keeps that log.
rm -f "$R/lib/preinit/05_mu300_early_recorder"
# fw4: the kernel has nftables (kernel/patches/0001-nftables-kmi.patch); before it, this service was off
[ -e "$R/etc/rc.d/S19firewall" ] || ln -sf ../init.d/firewall "$R/etc/rc.d/S19firewall"

echo mu5358 > "$R/etc/mu5358-device"

# The LAN address: mu300-lan-ip gives 90-mu300 (a first install) and preinit's early DHCP server the device's default,
# 192.168.78.1 for anything it does not know (the U30 Air's). This board's is 192.168.58.1, boot/init's subnet:
# 192.168.79.0/24 and the mu300 ones can be on the same computer.
f="$R/opt/mu300/bin/mu300-lan-ip"
if [ -f "$f" ] && ! grep -q mu5358 "$f"; then
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
# The release's zram-start asks busybox's swapon for --show, which it does not have: whether zram0 is on comes from
# /proc/swaps instead, as in mu300-linux's later mu300-zram
f="$R/opt/mu300/bin/zram-start"
[ -f "$f" ] && sed -i 's|swapon --show=NAME --noheadings \| grep -q zram0|grep -q "^/dev/zram0[[:space:]]" /proc/swaps|' "$f"
cat > "$R/etc/init.d/mu5358-hw" <<'EOF'
#!/bin/sh /etc/rc.common
# U30 Pro: the vendor modules of /etc/mu5358/modules (from /lib/mu5358/modules, in that order), then zram. In the
# background, as mu300-hw does: a module that hangs must not keep the consoles and the network from starting.
START=12

boot() {
	(
		for m in $(cat /etc/mu5358/modules); do
			grep -q "^$(echo $m | tr - _) " /proc/modules && continue
			/opt/mu300/bin/busybox insmod /lib/mu5358/modules/$m.ko && echo "ok $m" || echo "FAILED $m"
		done
		/opt/mu300/bin/zram-start && echo "zram on"
		# Wi-Fi: wlan0 appears after network (S20) started, so the hotspot comes up here. The first time, mu300's
		# openwrt-wifi-config makes it (5 GHz without DFS, country TR, WPA2 with a random password: uci
		# wireless.ap0.key) with this board's name.
		for i in $(seq 1 30); do [ -e /sys/class/net/wlan0 ] && break; sleep 1; done
		if [ -e /sys/class/net/wlan0 ]; then
			if [ ! -e /etc/mu300/wifi-configured ] && [ ! -e /etc/mu300/hotspot.conf ]; then
				mkdir -p /etc/mu300
				{ echo "SSID=U30Pro-$(tr -d : < /sys/class/net/wlan0/address | tail -c 5 | tr a-f A-F)"
				  echo COUNTRY=TR; } > /etc/mu300/hotspot.conf
			fi
			sh /opt/mu300/bin/openwrt-wifi-config
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
printf 'kernel.panic_on_oops=1\nkernel.panic=10\n' > "$R/etc/sysctl.d/90-mu5358.conf"

# The system's own log into the persistent log on the eMMC (boot/init's LOGSEC: kernel log in its first half, this
# in the second), read from Android after the next boot: the board has no serial console of its own.
cat > "$R/etc/init.d/mu5358-log" <<'EOF'
#!/bin/sh /etc/rc.common
# U30 Pro: the system - addresses, links, bridges, processes and logread - into sectors 122153984.. of mmcblk0
# (512 KiB: the second half of boot/init's persistent log at 122152960): every 5 s for the first five minutes, then
# every minute for as long as it runs (a USB loss on 2026-10-29 came after the five minutes; ~0.7 GB a day).
START=13

start() {
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
			dd if=/tmp/mu5358-log of=/dev/mmcblk0 bs=512 seek=122153984 count=1024 conv=notrunc,fsync 2>/dev/null
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

# The buttons (2026-10-30), from mu300's mu300-keys (presses as text lines): the board has no LEDs (a screen, later),
# so mu300-buttons' LED logic does not fit. gpio-keys: power 116, volume 114/115, "Wifi Switch Key" 102.
cat > "$R/opt/mu5358/bin/buttons" <<'EOF'
#!/bin/sh
# U30 Pro buttons: power held 3 s shuts down cleanly, a short press switches the screen's backlight off or on; the
# Wi-Fi key held 3 s switches the hotspot off or on again. Other presses are logged only.
# One reader only: a restart by procd stops this shell, and the old mu300-keys stays until its next write
for p in $(pidof mu300-keys); do kill $p; done
/opt/mu300/bin/mu300-keys -l 3000 | while read -r code what; do
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
done
EOF
chmod 755 "$R/opt/mu5358/bin/buttons"
cat > "$R/etc/init.d/mu5358-buttons" <<'EOF'
#!/bin/sh /etc/rc.common
# U30 Pro: the buttons (/opt/mu5358/bin/buttons)
START=95
USE_PROCD=1

start_service() {
	[ -x /opt/mu300/bin/mu300-keys ] || return 0
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
# u30ui turns the picture with the panel's MADCTL through this (tools/spispeed; without it the CPU turns it, and
# the frames tear)
M4=$(cd "$(dirname "$0")/.." && pwd)/tools/spispeed
[ -f "$M4/panelcmd.ko" ] && cp "$M4/panelcmd.ko" "$R/lib/mu5358/modules/
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
{ dmesg -w & logread -f; } 2>/dev/null | awk '
	/charger-manager|charger_policy|policy: |sprd-fgu|sc27xx-fgu|SPRD_PDBG|\|__|mmc0: r_speed|SFP:|AICWFDBG|thmzone|bq2560x|zte_power_supply|sprd_iommu|cm_batt_works|typec-pd/ { next }
	{ sub(/^\[ *[0-9]+\.[0-9]+\]\[[^]]*\] /, "["); print; fflush() }' | /opt/mu5358/bin/screencon
EOF
    chmod 755 "$R/opt/mu5358/bin/screen-console"
    cat > "$R/etc/init.d/mu5358-screen" <<'EOF'
#!/bin/sh /etc/rc.common
# U30 Pro: the screen - u30ui (the launcher's papatya, tools/u30ui), else the log as a console
# (/opt/mu5358/bin/screen-console)
START=11
USE_PROCD=1

start_service() {
	# DRM master is one client's: a screencon left by the console's pipe would keep u30ui off the panel
	for p in $(pidof screencon); do kill $p; done
	procd_open_instance screen
	if [ -x /opt/mu5358/bin/u30ui ]; then
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
f="$R/etc/hotplug.d/iface/10-mu300-usb"
if [ -f "$f" ] && ! grep -q mu5358 "$f"; then
    sed -i 's|^stamp=/tmp/mu300-usb-rebound$|# mu5358: not while the host has the link (carrier)\nip -o link show usb0 2>/dev/null \| grep -q LOWER_UP \&\& exit 0\n&|' "$f"
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
    ln -sf ../init.d/mu300-atd "$R/etc/rc.d/S19mu300-atd"
    cat > "$R/etc/uci-defaults/96-mu5358-wan" <<'EOF'
# U30 Pro: the wan on, now that the modem is ported (95-mu5358 had it off); the APN of the SIM it was brought up
# with (Turk Telekom) when none is set
uci -q set network.wan.auto='1'
[ -n "$(uci -q get network.wan.apn)" ] || uci -q set network.wan.apn='internet'
uci -q commit network
exit 0
EOF
fi
echo "customized $R"
