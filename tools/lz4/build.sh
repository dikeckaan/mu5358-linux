#!/bin/sh
# A static aarch64 lz4 for the device: the updater (openwrt/update/mu5358-update) packs the init_boot ramdisk
# LZ4-legacy (lz4 -l, what the kernel's initramfs decompressor wants, as magiskboot does from Android), and the base
# rootfs has no lz4. Built from the pinned release tarball in the pinned Alpine image (musl, static), like the
# installer's static tools in tools/make-release.sh. Output: tools/lz4/lz4 (not in the repository).
set -eu
V=1.10.0
SHA=537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
cd "$(dirname "$0")"
UID_GID=$(id -u):$(id -g)
docker run --rm --platform linux/arm64 -e V="$V" -e SHA="$SHA" -e UID_GID="$UID_GID" -v "$PWD":/w -w /tmp \
    alpine:3.20@sha256:d9e853e87e55526f6b2917df91a2115c36dd7c696a35be12163d44e6e2a4b6bc sh -eu -c '
    apk add -q build-base wget
    wget -q -O lz4.tar.gz "https://github.com/lz4/lz4/releases/download/v$V/lz4-$V.tar.gz"
    echo "$SHA  lz4.tar.gz" | sha256sum -c - >/dev/null
    tar -xzf lz4.tar.gz
    make -C "lz4-$V" -j4 lz4 LDFLAGS=-static >/dev/null
    strip "lz4-$V/programs/lz4"
    cp "lz4-$V/programs/lz4" /w/lz4
    chown "$UID_GID" /w/lz4'
file lz4 | grep -q 'statically linked' || { echo "lz4 is not static" >&2; exit 1; }
echo "tools/lz4/lz4: $(du -k lz4 | cut -f1) KiB, lz4 $V"
