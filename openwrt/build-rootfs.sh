#!/bin/bash
# Build the U30 Pro's OpenWrt root filesystem image (ext4, label mu5358root) from mu300-linux's openwrt-luci rootfs (its
# control panel, SMS pool, ttyd; OpenWrt 25.12.6 armsr/armv8), renamed to mu5358 (openwrt/rebrand.sh), and this kernel's modules. Runs in kernel/Dockerfile's container as root (so
# owners and modes survive):
#
#   docker run --rm -v $PWD:/w -v /path/to/mu300-openwrt-rootfs.tar.gz:/in/rootfs.tar.gz:ro \
#       -v /Volumes/mu5358kb/out:/kout:ro mu5358-kbuild bash /w/openwrt/build-rootfs.sh
# (2026-10-30: mu300-linux release v2026.10.26's mu300-openwrt-rootfs.tar.gz; any image with mke2fs will do)
#
# Out: /w/work/openwrt/mu5358root.img.gz (IMG_NAME), and next to it .image (the image's sha256 and size) - the filesystem at the size of the whole region (SIZE_MIB, default 27808:
# sectors 65200128 up to the persistent log at 122152960), gzipped: there is no resize2fs on the board, and the
# empty blocks compress to nothing. Written with `gunzip | dd` at the region's offset.
#
# The board's own changes to the tree are openwrt/customize.sh's (it also patches an installed system from Android).
set -e
command -v mke2fs >/dev/null || { apt-get update -qq && apt-get install -y -qq e2fsprogs >/dev/null; }
R=/build/root
OUT=/w/work/openwrt
KREL=$(cat /kout/include/config/kernel.release)
rm -rf $R && mkdir -p $R $OUT
tar -xzf /in/rootfs.tar.gz -C $R
# mu300's names become this project's (openwrt/rebrand.sh), and the panel's Turkish and Chinese catalogs are built
# again from the renamed .po sources (openwrt/panel/po, tools/po2lmo.py: mu300's)
sh /w/openwrt/rebrand.sh $R
CAT=$R/usr/lib/lua/luci/i18n
rm -f $CAT/mu5358.*.lmo
python3 /w/tools/po2lmo.py /w/openwrt/panel/po/tr/mu5358.po $CAT/mu5358.tr.lmo
python3 /w/tools/po2lmo.py /w/openwrt/panel/po/zh_Hans/mu5358.po $CAT/mu5358.zh-cn.lmo
[ -s $CAT/mu5358.tr.lmo ] && [ -s $CAT/mu5358.zh-cn.lmo ] || { echo "the panel's catalogs were not built" >&2; exit 1; }

# kernel modules: the 5.4 ones of the F50 out. This kernel's own (GKI in-tree) modules wait in
# /root/gki-modules-disabled: with them in /lib/modules, kmodloader loaded everything /etc/modules.d names at boot
# and the board panicked a few seconds into OpenWrt (2026-10-29); they go back one at a time.
rm -rf $R/lib/modules/*
mkdir -p $R/root/gki-modules-disabled
find /kout -name '*.ko' -not -path '*/scripts/*' -exec cp {} $R/root/gki-modules-disabled/ \;
echo "modules: $(ls $R/root/gki-modules-disabled | wc -l) for $KREL, not installed"

# hardware services off, the LAN address, the board's services; the vendor modules, the AIC8800 firmware and this
# board's Wi-Fi calibration and MAC (prodnv's) from work/ - copied from the device, not in this repository
# GENERIC=1 (the release, tools/make-release.sh): none of them - the installer adds this device's own on the device
# (installer/mu5358-install.sh, vendor_files)
if [ -n "${GENERIC:-}" ]; then
    VERSION="${VERSION:-}" sh /w/openwrt/customize.sh $R "" /nonexistent /nonexistent
else
    sh /w/openwrt/customize.sh $R /w/work/unit2/dlkm/vendor_dlkm/lib/modules /w/work/firmware/aic8800 /w/work/prodnv
fi

# the modem chroot's Android files (modem/README.md: work/android-subset, the property area as dev/__properties__),
# root's, directories and programs 0755, the property files 0444, the rest 0644
if [ -z "${GENERIC:-}" ] && [ -d /w/work/android-subset/vendor/bin ]; then
    A=$R/opt/mu5358/android
    mkdir -p $A
    cp -a /w/work/android-subset/. $A/
    rm -rf $A/dev-properties
    mv $A/dev/__properties__ $A/dev-properties
    rmdir $A/dev 2>/dev/null || true
    chown -R 0:0 $A
    find $A -type d -exec chmod 755 {} +
    find $A -type f -path '*/bin/*' -exec chmod 755 {} +
    find $A -type f ! -path '*/bin/*' -exec chmod 644 {} +
    find $A/dev-properties -type f -exec chmod 444 {} +
    echo "modem chroot: $(du -sh $A | cut -f1)"
else
    echo "no work/android-subset: the image has no modem chroot (modem/README.md)"
fi

size=${SIZE_MIB:-27808}
img=/build/mu5358root.img
gz=$OUT/${IMG_NAME:-mu5358root.img.gz}
rm -f $img $gz
mke2fs -q -t ext4 -L mu5358root -O ^metadata_csum_seed -E root_owner=0:0,lazy_itable_init=0 -d $R $img ${size}M
e2fsck -fn $img >/dev/null
gzip -${GZIP_LEVEL:-1} -n -c $img > $gz
echo "image: ${size} MiB, $(du -m $gz | cut -f1) MiB gzipped, sha256 $(sha256sum < $img | cut -c1-64)"
echo "$(sha256sum < $img | cut -c1-64) $(stat -c %s $img)" > $gz.image
# on a Linux host the container is root and the caller is not (the Release workflow): the output is the caller's
[ -z "${HOST_UID:-}" ] || chown -R "$HOST_UID:${HOST_GID:-$HOST_UID}" $OUT
