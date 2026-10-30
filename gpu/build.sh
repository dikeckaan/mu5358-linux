#!/bin/sh
# Build gltest for bionic with plain clang and lld (no NDK). $1 = a copy of the Android tree (work/android-subset:
# libc.so and libdl.so from the runtime apex, egl/libGLES_mali.so from /vendor). cltest is mu300's binary for
# Android 13's Mali; Android 15's libGLES_mali.so has no OpenCL, so it cannot link there.
set -e
A=$1
B=$A/apex/com.android.runtime/lib64/bionic
cd "$(dirname "$0")"
CC="clang --target=aarch64-linux-android29 -O2 -fPIE -nostdlib -ffreestanding -fuse-ld=lld -Wl,-pie
  -Wl,--dynamic-linker=/system/bin/linker64 -Wl,--allow-shlib-undefined -Wl,-z,max-page-size=4096"
$CC crt_android.c gltest.c "$A/vendor/lib64/egl/libGLES_mali.so" "$B/libc.so" "$B/libdl.so" -o gltest
