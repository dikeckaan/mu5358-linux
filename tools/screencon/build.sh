#!/bin/bash
# Build screencon (static, aarch64) in mu5358-mainline-build, the font from the kernel tree in the mu5358-mainline
# volume:
#   docker run --rm -v mu5358-mainline:/src -v "$PWD/tools/screencon":/w mu5358-mainline-build bash /w/build.sh
set -e
dpkg -s libc6-dev-arm64-cross >/dev/null 2>&1 ||
    { apt-get update -qq && apt-get install -y -qq libc6-dev-arm64-cross linux-libc-dev-arm64-cross >/dev/null; }
F=${FONT:-/src/linux-6.18.59/lib/fonts/font_6x10.c}   # the Release workflow names the GKI tree's copy, the same file
{
    echo "/* from the kernel's $(basename $F) (GPL-2.0) */"
    echo "static const unsigned char fontdata_6x10[] = {"
    sed -n '/{ 0, 0, FONTDATAMAX, 0 }, {/,/^} };/p' $F | sed '1d;$d'
    echo "};"
} > /tmp/font_6x10.h
aarch64-linux-gnu-gcc -O2 -Wall -static -I/tmp -o /w/screencon /w/screencon.c
aarch64-linux-gnu-strip /w/screencon
ls -la /w/screencon
