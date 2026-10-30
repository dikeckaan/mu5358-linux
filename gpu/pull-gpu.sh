#!/bin/bash
# Pull the Mali userspace (libGLES_mali.so: GLES, OpenCL and Vulkan in one) and its library closure from the running
# Android into the modem chroot's tree (work/android-subset), as mu300's extract-gpu-subset.sh did for Android 13.
# The closure is followed with readelf (in mu5358-mainline-build) until nothing is missing; the libraries the modem
# daemons already brought are not pulled again. Proprietary: none of it goes into this repository.
#   ANDROID_SERIAL=<serial> gpu/pull-gpu.sh
set -e
cd "$(dirname "$0")/.."
T=work/android-subset
SEARCH="/vendor/lib64/egl /vendor/lib64 /vendor/lib64/hw /system/lib64 /apex/com.android.runtime/lib64/bionic"
pull() {   # pull /abs/path -> $T/abs/path (via a root copy: some vendor files are not world readable)
    [ -e "$T$1" ] && return 0
    mkdir -p "$T$(dirname "$1")"
    adb shell "su -c 'cp $1 /data/local/tmp/gpu.so && chmod 644 /data/local/tmp/gpu.so'" </dev/null
    adb pull /data/local/tmp/gpu.so "$T$1" >/dev/null
}
find_lib() {   # the first directory on the device that has $1
    for d in $SEARCH; do
        adb shell "[ -e $d/$1 ] && echo $d/$1" </dev/null | tr -d '\r' | grep . && return 0
    done
    return 1
}
pull /vendor/lib64/egl/libGLES_mali.so
for round in $(seq 1 10); do
    missing=$(docker run --rm -v "$PWD/$T":/s:ro -w /s mu5358-mainline-build bash -c '
        dirs="vendor/lib64/egl vendor/lib64 vendor/lib64/hw system/lib64 apex/com.android.runtime/lib64/bionic"
        for f in $(find . -name "*.so"); do
            readelf -d "$f" 2>/dev/null | sed -n "s/.*NEEDED.*\[\(.*\)\]/\1/p"
        done | sort -u | while read l; do
            ok=; for d in $dirs; do [ -e "$d/$l" ] && ok=1; done; [ -n "$ok" ] || echo "$l"
        done')
    [ -n "$missing" ] || { echo "closure complete after $round rounds"; break; }
    for l in $missing; do
        p=$(find_lib "$l") || { echo "not on the device: $l" >&2; continue; }
        echo "pull $p"; pull "$p"
    done
done
# OpenCL programs ask for libOpenCL.so: on Mali it is libGLES_mali.so itself
ln -sfn egl/libGLES_mali.so $T/vendor/lib64/libOpenCL.so
du -sh $T/vendor/lib64/egl $T
