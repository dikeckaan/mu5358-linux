#!/bin/bash
# Build u30ui (static, aarch64) and its preview (host: every page as a PNG with sample data) in mu5358-mainline-build:
#   docker run --rm -v "$PWD/tools/u30ui":/w mu5358-mainline-build bash /w/build.sh [preview]
set -e
cd /w
dpkg -s libc6-dev-arm64-cross >/dev/null 2>&1 ||
    { apt-get update -qq && apt-get install -y -qq libc6-dev-arm64-cross linux-libc-dev-arm64-cross >/dev/null; }
SRC="ui.c gfx.c icons.c display.c input.c data.c png.c qr.c"
if [ "$1" = preview ]; then
    command -v gcc >/dev/null || { apt-get update -qq && apt-get install -y -qq gcc libc6-dev >/dev/null; }
    gcc -O2 -Wall -Wno-unused-function -o /tmp/u30ui-preview preview.c gfx.c icons.c png.c qr.c -lm
    mkdir -p preview && cd preview && /tmp/u30ui-preview
    ls -la /w/preview
    exit 0
fi
aarch64-linux-gnu-gcc -O2 -Wall -Wno-format-truncation -Wno-unused-function -static -o u30ui $SRC -lm -lpthread
aarch64-linux-gnu-strip u30ui
ls -la u30ui
