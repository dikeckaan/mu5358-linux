#!/bin/sh
# Stage the website as it is published: site/ without site/tools.
#   site/tools/stage.sh OUT
# .github/workflows/pages.yml publishes OUT; for a look on this computer:
#   sh site/tools/stage.sh /tmp/mu5358-site && python3 -m http.server -d /tmp/mu5358-site 8358
set -eu
TOP=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:?usage: site/tools/stage.sh OUT}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
case $OUT in "$TOP/site"|"$TOP/site"/*|"$TOP") echo "OUT must not be the site itself or the repository" >&2; exit 1 ;; esac
rm -rf "$OUT/assets" "$OUT/tr" "$OUT/zh" "$OUT/web-installer"
(cd "$TOP/site" && tar -cf - --exclude ./tools .) | (cd "$OUT" && tar -xf -)
echo "staged: $OUT ($(find "$OUT" -type f | wc -l | tr -d ' ') files)"
