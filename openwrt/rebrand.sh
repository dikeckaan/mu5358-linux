#!/bin/sh
# mu300-linux's OpenWrt tree (the openwrt-luci release tarball, unpacked at $1) as MU5358 Linux's: every mu300 name
# becomes mu5358 - files, directories, links, and the text inside (paths, commands, services, uci sections, the
# panel's ubus objects and its catalogs) - so the tools, the control panel and the services run here under this
# project's name. build-rootfs.sh runs it on the fresh tree, before customize.sh. Nothing in the tree is an ELF
# or other binary that carries the name (checked on v2026.10.29.1: none), so a text rewrite is the whole rename;
# the panel's compiled catalogs (.lmo) are built again from the .po sources by build-rootfs.sh.
#
# Dropped here, before the rename: the services of the F50 / U30 Air hardware and the ones this project forbids
# (the slot switch and the boot re-arm); every other mu300 service loses its rc.d link, and customize.sh links the
# ones this board runs. External names that are not ours go back: the VPN module's repository, the Telegram channel.
set -e
R=${1:?usage: rebrand.sh ROOT}
[ -d "$R/opt/mu300/bin" ] || { echo "$R: no /opt/mu300/bin - not a mu300-linux tree" >&2; exit 1; }

# 1. what goes: the hardware services of other boards (LEDs, their buttons, the vendor chroot, the CPU voltage),
# mu300-post (it re-arms the boot slot) and mu300-hw (this board's is customize.sh's). The files, so that nothing
# can start them.
for s in mu300-hw mu300-post mu300-vendor mu300-buttons mu300-cpu-voltage; do
    rm -f "$R/etc/init.d/$s" "$R"/etc/rc.d/[SK]??"$s"
done
# every other mu300 service: no rc.d link; customize.sh links the ones this board runs
rm -f "$R"/etc/rc.d/[SK]??mu300-*
rm -f "$R/lib/preinit/04_mu300_early_leds"
# the early recorder writes 48 MiB into the boot partition of the slot Linux runs from (boot_b here: our kernel's)
rm -f "$R/lib/preinit/05_mu300_early_recorder"
# the tools that write boot partitions or misc for the F50 / U30 Air: a refusal in their place (customize.sh's
# mu5358-install.sh is the installer and updater here)
for t in mu300-next-boot mu300-update early-recorder; do
    [ -e "$R/opt/mu300/bin/$t" ] || continue
    printf '#!/bin/sh\necho "%s: not on the U30 Pro (MU5358 Linux): it writes boot partitions or misc" >&2\nexit 1\n' \
        "${t#mu300-}" | sed 's/^echo "\([a-z-]*\):/echo "mu5358-\1:/' > "$R/opt/mu300/bin/$t"
    chmod 755 "$R/opt/mu300/bin/$t"
done

# 2. the names: files and directories, deepest first; then the links' targets
find "$R" -depth -name '*mu300*' | while read -r p; do
    d=$(dirname "$p"); b=$(basename "$p")
    mv "$p" "$d/$(printf %s "$b" | sed 's/mu300/mu5358/g')"
done
find "$R" -type l | while read -r l; do
    t=$(readlink "$l")
    case $t in *mu300*) ln -sfn "$(printf %s "$t" | sed 's/mu300/mu5358/g')" "$l" ;; esac
done

# 3. the text: every text file that says mu300, MU300 or Mu300 (grep -I leaves binaries out: the .lmo catalogs,
# the compressed and the ELF files are never touched)
grep -rIl --exclude-dir=proc --exclude-dir=sys 'mu300\|MU300\|Mu300' "$R" | while read -r f; do
    [ -L "$f" ] && continue
    sed -i 's/mu300/mu5358/g; s/MU300/MU5358/g; s/Mu300/Mu5358/g' "$f"
done
# what is not ours: the VPN module's repository and the Telegram channel are mu300-linux's
grep -rIl 'mu5358-linux-vpn\|mu5358project\|mu5358-linux-mirror' "$R" | while read -r f; do
    [ -L "$f" ] && continue
    sed -i 's/mu5358-linux-vpn/mu300-linux-vpn/g; s/mu5358project/mu300project/g; s/mu5358-linux-mirror/mu300-linux-mirror/g' "$f"
done

# 4. the board's name: mu5358-device answers mu5358 on this board (/etc/mu5358-device, customize.sh's marker)
f="$R/opt/mu5358/bin/mu5358-device"
if [ -f "$f" ] && ! grep -q 'echo mu5358; return' "$f"; then
    sed -i '/^device() {$/a\
    [ -e "$R/etc/mu5358-device" ] \&\& { echo mu5358; return; }' "$f"
fi

# 5. the old names as links, for a user's own script or a path missed above
ln -sfn mu5358 "$R/opt/mu300"
[ -e "$R/etc/mu5358" ] && ln -sfn mu5358 "$R/etc/mu300"
echo "rebranded: $(find "$R" -name '*mu5358*' | wc -l) names, $(grep -rIl 'mu5358' "$R" | wc -l) files"
