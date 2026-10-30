#!/bin/sh
# The CJK font the screen carries: Noto Sans SC (OFL, work/fonts/NotoSansSC-VF.ttf from notofonts/noto-cjk), as a
# static instance at weight 400 and 500, cut down to the characters i18n.h uses (plus digits, Latin and the
# punctuation the pages draw), as C arrays in font_cjk.h. Roboto has no CJK: gfx.c falls back to these for any
# character Roboto lacks. ~300 glyphs, some 200 KB; the whole font would be 17 MB.
#   tools/u30ui2/mkfont.sh         (docker; fonttools in a throwaway Ubuntu container)
set -e
cd "$(dirname "$0")"
VF=../../work/fonts/NotoSansSC-VF.ttf
[ -f "$VF" ] || { echo "missing $VF: curl -fsSL -o $VF https://github.com/notofonts/noto-cjk/raw/main/Sans/Variable/TTF/Subset/NotoSansSC-VF.ttf" >&2; exit 1; }
# every non-ASCII character of the strings, one code point a line
python3 - > /tmp/cjk-chars.txt <<'PY'
import re
s = open('i18n.h', encoding='utf-8').read()
cps = {ord(c) for c in s if ord(c) > 127}
cps |= {ord(c) for c in '…·°→↑↓▲▼×•—“”‘’、。，：；！？（）%'}
cps |= set(range(0x20, 0x7f))
print('\n'.join('U+%04X' % c for c in sorted(cps)))
PY
echo "$(wc -l < /tmp/cjk-chars.txt) code points"
docker run --rm -v "$(cd ../../work/fonts && pwd)":/f:ro -v /tmp/cjk-chars.txt:/chars.txt:ro -v "$PWD":/o ubuntu:24.04 bash -ec '
apt-get update -qq >/dev/null && apt-get install -y -qq python3-fonttools >/dev/null 2>&1
cd /tmp
for w in 400 500; do
    python3 -m fontTools.varLib.instancer -q -o inst$w.ttf /f/NotoSansSC-VF.ttf wght=$w
    python3 -m fontTools.subset inst$w.ttf --unicodes-file=/chars.txt --output-file=sub$w.ttf --no-hinting --desubroutinize \
        --layout-features="" --name-IDs="" --notdef-outline
    ls -la sub$w.ttf
done
{
    echo "/* Noto Sans SC (OFL-1.1, third_party/LICENSE-NotoSansSC.txt), subset by mkfont.sh: weight 400 and 500 */"
    for w in 400 500; do
        echo "static const unsigned char noto_sc_$w[] = {"
        od -An -tu1 -v sub$w.ttf | tr -s " " | sed "s/^ //; s/ /,/g; s/$/,/"
        echo "};"
    done
} > /o/font_cjk.h
'
ls -la font_cjk.h
