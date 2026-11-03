#!/bin/bash
# Build mu5358-nfc (static, aarch64) in mu5358-mainline-build, the image tools/u30ui2/build.sh uses:
#   sh tools/nfc/build.sh            -> tools/nfc/mu5358-nfc (git-ignored; tools/make-release.sh wants it)
set -e
cd "$(dirname "$0")"
docker run --rm -v "$PWD":/t -w /t mu5358-mainline-build sh -c '
    aarch64-linux-gnu-gcc -O2 -Wall -Wextra -static -o mu5358-nfc mu5358-nfc.c && aarch64-linux-gnu-strip mu5358-nfc'
ls -la mu5358-nfc
