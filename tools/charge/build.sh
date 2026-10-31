#!/bin/bash
# Build mu5358-charge (static, aarch64) in mu5358-mainline-build, as tools/u30ui2/build.sh does:
#   docker run --rm -v "$PWD/tools":/t mu5358-mainline-build bash /t/charge/build.sh
# tests/test_charge.py builds the same source for the host and runs it against fake sysfs trees.
set -e
cd /t/charge
dpkg -s libc6-dev-arm64-cross >/dev/null 2>&1 ||
    { apt-get update -qq && apt-get install -y -qq libc6-dev-arm64-cross linux-libc-dev-arm64-cross >/dev/null; }
aarch64-linux-gnu-gcc -O2 -Wall -Wextra -static -o mu5358-charge mu5358-charge.c
aarch64-linux-gnu-strip mu5358-charge
ls -la mu5358-charge
