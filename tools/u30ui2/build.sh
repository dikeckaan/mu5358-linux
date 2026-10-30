#!/bin/bash
# Build u30ui2 (static, aarch64) and its preview (host: every page in the three languages as PNG files with sample
# data) in mu5358-mainline-build. gfx, icons, display, input, png and qr are tools/u30ui's (the classic UI's),
# built here with HAVE_CJK for the Chinese glyphs (font_cjk.h, mkfont.sh):
#   docker run --rm -v "$PWD/tools":/t mu5358-mainline-build bash /t/u30ui2/build.sh [preview]
set -e
cd /t/u30ui2
dpkg -s libc6-dev-arm64-cross >/dev/null 2>&1 ||
    { apt-get update -qq && apt-get install -y -qq libc6-dev-arm64-cross linux-libc-dev-arm64-cross >/dev/null; }
U=../u30ui
SHARED="$U/gfx.c $U/icons.c $U/png.c $U/qr.c"
CFLAGS="-O2 -Wall -Wno-format-truncation -Wno-unused-function -DHAVE_CJK -I$U -I/t/u30ui2"
if [ "$1" = preview ]; then
    command -v gcc >/dev/null || { apt-get update -qq && apt-get install -y -qq gcc libc6-dev >/dev/null; }
    gcc $CFLAGS -o /tmp/u30ui2-preview preview.c i18n.c $SHARED -lm
    mkdir -p preview && cd preview && /tmp/u30ui2-preview
    ls /t/u30ui2/preview | wc -l
    exit 0
fi
aarch64-linux-gnu-gcc $CFLAGS -static -o u30ui2 ui.c i18n.c data.c $U/display.c $U/input.c $SHARED -lm -lpthread
aarch64-linux-gnu-strip u30ui2
ls -la u30ui2
