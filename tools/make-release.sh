#!/bin/sh
# Build a MU5358 Linux release for the web installer (site/web-installer, installer/mu5358-install.sh), in
# work/release/TAG:
#
#   tools/make-release.sh TAG [MU300_OPENWRT_ROOTFS_TAR_GZ]
#
#   mu5358-openwrt.img.gz    the system: ext4 of 2047 MiB (fulldumpdb less its last MiB), label mu5358root, from
#                            mu300-linux's openwrt-luci rootfs (default: its release v2026.10.29.1, checked against the release asset digest on GitHub) and openwrt/customize.sh,
#                            without any of a device's own files (GENERIC=1: no vendor modules, firmware, Wi-Fi
#                            calibration, modem or GPU userspace - the installer copies them from the device itself)
#   mu5358-kernel.Image.gz   the GKI rebuild (kernel/), from the build volume's out/
#   mu5358-ramdisk.tar.gz    the init_boot ramdisk's own files: init (boot/init), mu/busybox, mu/e2fsck (tools/e2fsck); the device adds
#                            its misc block and sprd_pmic_wdt.ko and puts it into a copy of its own init_boot_a
#   mu5358-tools.tar.gz      installer/gpt-userdata and f2fs-shrink (tools/f2fs-shrink.c), static for Android (NDK)
#   mu5358-install.sh, android-files.txt   the device side of the installer
#   SHA256SUMS               all of them, with "# version TAG" and "# image NAME SHA256 SIZE" (the uncompressed
#                            image, for the read back of fulldumpdb)
#   SHA256SUMS.sig           its usign (Ed25519) signature by the project's release key (tools/sign-release.sh;
#                            the key: MU5358_RELEASE_KEY_FILE, default ~/.config/mu5358-linux/release.sec), checked
#                            against installer/release.pub before the release counts; no key: no signature, and a
#                            warning
#
# Needs: docker with the mu5358-kbuild image (kernel/Dockerfile) and the kernel's build volume (KOUT), the built
# tools (tools/u30ui/u30ui, tools/screencon/screencon, tools/spispeed/panelcmd.ko, gpu/gltest), the busybox of
# mu300-linux's kernel release (BUSYBOX).
set -eu
cd "$(dirname "$0")/.."
TAG=${1:?usage: make-release.sh TAG [ROOTFS_TAR_GZ]}
ROOTFS=${2:-/Volumes/mu300rel12/r30/release/v2026.10.29.1/mu300-openwrt-luci-rootfs.tar.gz}
KOUT=${KOUT:-/Volumes/mu5358kb/out}
BUSYBOX=${BUSYBOX:-../mu300-linux/work/release/v2026.10.16/kernel/busybox}
OUT=work/release/$TAG
for f in "$ROOTFS" "$KOUT/arch/arm64/boot/Image" "$BUSYBOX" tools/u30ui/u30ui tools/u30ui2/u30ui2 tools/screencon/screencon \
         tools/spispeed/panelcmd.ko gpu/gltest work/e2fsck/e2fsck; do
    [ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
done
# the base, by its published digest (gh api repos/dikeckaan/mu300-linux/releases/tags/v2026.10.29.1)
ROOTFS_SHA256=${ROOTFS_SHA256:-7d4cde4024039d7778ed027e27137e0e2605b0507bba645a01a9715e069b1e5a}
[ "$(shasum -a 256 "$ROOTFS" | cut -c1-64)" = "$ROOTFS_SHA256" ] || { echo "$ROOTFS: not the release's rootfs" >&2; exit 1; }
rm -rf "$OUT" && mkdir -p "$OUT"
ROOTFS=$(cd "$(dirname "$ROOTFS")" && pwd)/$(basename "$ROOTFS")

docker run --rm -e GENERIC=1 -e VERSION="$TAG" -e SIZE_MIB=2047 -e IMG_NAME=mu5358-openwrt.img.gz -e GZIP_LEVEL=6 \
    -v "$PWD":/w -v "$ROOTFS":/in/rootfs.tar.gz:ro -v "$KOUT":/kout:ro mu5358-kbuild bash /w/openwrt/build-rootfs.sh
mv work/openwrt/mu5358-openwrt.img.gz work/openwrt/mu5358-openwrt.img.gz.image "$OUT/"

gzip -9 -n -c "$KOUT/arch/arm64/boot/Image" > "$OUT/mu5358-kernel.Image.gz"
# the ramdisk's files, root's (the installer runs cpio as root over what it unpacks)
docker run --rm -v "$PWD":/w -v "$(cd "$(dirname "$BUSYBOX")" && pwd)/busybox":/bb:ro -w /tmp mu5358-kbuild sh -c "
    mkdir -p rd/mu && cp /w/boot/init rd/init && cp /bb rd/mu/busybox && cp /w/work/e2fsck/e2fsck rd/mu/e2fsck &&
    chmod 755 rd/init rd/mu/busybox rd/mu/e2fsck &&
    tar --owner=0 --group=0 --numeric-owner --mtime=@0 --sort=name -czf /w/$OUT/mu5358-ramdisk.tar.gz -C rd init mu"
NDKCC=${NDKCC:-$(ls -d ~/Library/Android/sdk/ndk/*/ | tail -1)toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android30-clang}
"$NDKCC" -static -O2 -Wall -s -o installer/gpt-userdata installer/gpt-userdata.c
"$NDKCC" -static -O2 -Wall -s -o installer/f2fs-shrink tools/f2fs-shrink.c
tar --uid 0 --gid 0 -czf "$OUT/mu5358-tools.tar.gz" -C installer gpt-userdata f2fs-shrink
cp installer/mu5358-install.sh installer/android-files.txt "$OUT/"

cd "$OUT"
{
    echo "# version $TAG"
    echo "# image mu5358-openwrt.img.gz $(cat mu5358-openwrt.img.gz.image)"
    sha256sum mu5358-openwrt.img.gz mu5358-kernel.Image.gz mu5358-ramdisk.tar.gz mu5358-tools.tar.gz mu5358-install.sh \
        android-files.txt
} > SHA256SUMS
rm mu5358-openwrt.img.gz.image
cd - >/dev/null
KEY=${MU5358_RELEASE_KEY_FILE:-$HOME/.config/mu5358-linux/release.sec}
if [ -s "$KEY" ]; then
    MU5358_RELEASE_KEY_FILE=$KEY MU5358_OPENSSL=${MU5358_OPENSSL:-$(ls /opt/homebrew/bin/openssl 2>/dev/null || echo openssl)} \
        sh tools/sign-release.sh "$OUT/SHA256SUMS" installer/release.pub
    python3 -I tools/usign-verify.py installer/release.pub "$OUT/SHA256SUMS.sig" "$OUT/SHA256SUMS"
else
    echo "WARNING: no release key at $KEY: SHA256SUMS is not signed" >&2
fi
ls -la "$OUT"
cat "$OUT/SHA256SUMS"
