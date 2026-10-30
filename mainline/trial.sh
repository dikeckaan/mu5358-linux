#!/bin/bash
# One mainline trial on the U30 Pro, from the Mac over adb (Android running):
#   mainline/trial.sh NAME [IMAGE]     IMAGE: a kernel Image (default mainline/out/Image)
# boot_b gets the stock boot image with IMAGE as its kernel and misc the one-shot slot b block; after the reboot the
# kernel runs until the PMIC watchdog resets it (~5 min, panic=0), LK saves the minidump and rolls back to Android.
# Then the log comes out of sysdumpdb into work/mainline/NAME-kmsg.txt, and boot_b (GKI) and misc (Android) are
# put back. Never touches slot a or NV.
set -euo pipefail
cd "$(dirname "$0")/.."
NAME=${1:?usage: mainline/trial.sh NAME [IMAGE]}
IMG=${2:-mainline/out/Image}
# the bench unit: its adb serial, so no other U30 Pro plugged in is ever written
export ANDROID_SERIAL=${MU5358_SERIAL:?set MU5358_SERIAL to the bench unit's adb serial}
W=work/mainline
GKI=work/unit2/linux-b/boot.img GKI_SHA=9ddb7794334a5a0c
su_do() { adb shell "su -c '$1'" </dev/null | tr -d '\r'; }
[ "$(adb shell getprop ro.serialno </dev/null | tr -d '\r')" = "$MU5358_SERIAL" ] || { echo "not the bench unit" >&2; exit 1; }
[ "$(su_do 'readlink -f /dev/block/by-name/boot_b')" = /dev/block/mmcblk0p45 ] || exit 1
[ "$(su_do 'readlink -f /dev/block/by-name/misc')" = /dev/block/mmcblk0p4 ] || exit 1

python3 -I mainline/mkboot.py work/unit2/img/boot_a.img "$IMG" $W/boot-$NAME.img >/dev/null
want=$(shasum -a256 $W/boot-$NAME.img | cut -c1-16)
adb push $W/boot-$NAME.img /data/local/tmp/b.img >/dev/null
adb push work/unit2/linux-b/misc-bc-b-trial.bin /data/local/tmp/bc.bin >/dev/null
got=$(su_do 'dd if=/data/local/tmp/b.img of=/dev/block/mmcblk0p45 bs=1M conv=fsync 2>/dev/null; rm /data/local/tmp/b.img; sha256sum /dev/block/mmcblk0p45 | cut -c1-16')
[ "$got" = "$want" ] || { echo "boot_b: $got, want $want" >&2; exit 1; }
bc=$(su_do '[ $(wc -c < /data/local/tmp/bc.bin) = 32 ] && dd if=/data/local/tmp/bc.bin of=/dev/block/mmcblk0p4 bs=1 seek=2048 count=32 conv=notrunc,fsync 2>/dev/null; rm /data/local/tmp/bc.bin; dd if=/dev/block/mmcblk0p4 bs=1 skip=2048 count=32 2>/dev/null | xxd -p | tr -d "\n"')
[ "$bc" = "$(xxd -p work/unit2/linux-b/misc-bc-b-trial.bin | tr -d '\n')" ] || { echo "misc: $bc" >&2; exit 1; }
# what the eMMC holds, not the page cache: once LK read the Android block although this read back the trial one
sleep 3
bc=$(su_do 'sync; echo 3 > /proc/sys/vm/drop_caches; dd if=/dev/block/mmcblk0p4 bs=1 skip=2048 count=32 2>/dev/null | xxd -p | tr -d "\n"')
[ "$bc" = "$(xxd -p work/unit2/linux-b/misc-bc-b-trial.bin | tr -d '\n')" ] || { echo "misc on the eMMC: $bc" >&2; exit 1; }
echo "$(date +%T) $NAME: boot_b $want, misc armed, rebooting"
adb reboot
sleep 30
for i in $(seq 200); do
    [ "$(adb shell getprop sys.boot_completed 2>/dev/null </dev/null | tr -d '\r')" = 1 ] && break
    sleep 3
done
[ "$(adb shell getprop sys.boot_completed 2>/dev/null </dev/null | tr -d '\r')" = 1 ] || { echo "Android did not come back" >&2; exit 1; }
sleep 5
echo "$(date +%T) $NAME: Android back"
su_do 'cat /dev/block/by-name/sysdumpdb > /data/local/tmp/sd.bin'
adb pull /data/local/tmp/sd.bin $W/sysdumpdb-$NAME.bin >/dev/null
su_do 'rm /data/local/tmp/sd.bin'
X=$W/sdx-$NAME; rm -rf $X
python3 -I tools/sysdumpdb-extract.py $W/sysdumpdb-$NAME.bin $X >/dev/null
t=$(ls $X/*kmsg_text* 2>/dev/null | head -1); r=$(ls $X/*printk_ring* 2>/dev/null | head -1)
if [ -n "$t" ] && [ "$(tr -d '\0' < "$t" | wc -c)" -gt 100 ]; then tr -d '\0' < "$t" > $W/$NAME-kmsg.txt
elif [ -n "$r" ]; then LC_ALL=C strings -n 6 "$r" > $W/$NAME-kmsg.txt
else echo "no minidump sections" > $W/$NAME-kmsg.txt; fi
# GKI and the Android block back
adb push $GKI /data/local/tmp/b.img >/dev/null
adb push work/unit2/linux-b/misc-bc-android.bin /data/local/tmp/bc.bin >/dev/null
got=$(su_do 'dd if=/data/local/tmp/b.img of=/dev/block/mmcblk0p45 bs=1M conv=fsync 2>/dev/null; rm /data/local/tmp/b.img; sha256sum /dev/block/mmcblk0p45 | cut -c1-16')
bc=$(su_do '[ $(wc -c < /data/local/tmp/bc.bin) = 32 ] && dd if=/data/local/tmp/bc.bin of=/dev/block/mmcblk0p4 bs=1 seek=2048 count=32 conv=notrunc,fsync 2>/dev/null; rm /data/local/tmp/bc.bin; dd if=/dev/block/mmcblk0p4 bs=1 skip=2048 count=32 2>/dev/null | xxd -p | tr -d "\n"')
echo "$(date +%T) $NAME: restored boot_b $got (want $GKI_SHA), misc $bc"
echo "log: $W/$NAME-kmsg.txt ($(wc -l < $W/$NAME-kmsg.txt) lines)"
