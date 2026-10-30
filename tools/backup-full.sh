#!/bin/sh
# Back up EVERYTHING of a rooted ZTE U30 Pro (MU5358) before porting to it: every partition by name (userdata and
# super included), the eMMC boot areas (spl_a/spl_b live there) and both GPT copies, each checked against a SHA-256
# taken on the device. Adapted from mu300-linux/tools/backup-full.sh.
#
#   tools/backup-full.sh OUTDIR            (ANDROID_SERIAL=... picks the device; adb over TCP works too)
#
# Partitions are read in 512 MiB pieces, gzipped on the device, pulled and removed there, so a partition larger
# than the free space of /data is not a problem, and no binary ever goes through `adb exec-out "su -c ..."` (a
# pty there can rewrite LF as CRLF). The pieces of one partition are gzip members of one file:
# OUTDIR/<name>.img.gz unpacks with plain gunzip. SHA256SUMS lists the raw images, as the device computed them.
# The U30 Pro's /dev/block/by-name also names the whole disk (mmcblk0) and aliases the boot areas
# (spl_a = mmcblk0boot0): the whole disk is skipped, each block device is dumped once under its first name.
# userdata is copied while Android runs, so it is only as consistent as a power cut; everything else is static.
# Nothing here belongs in a public repository: the dumps contain device identifiers and personal data.
set -eu
OUT=${1:?usage: tools/backup-full.sh OUTDIR}
CHUNK=512            # MiB per piece
T=/data/local/tmp/mu5358-backup
command -v adb >/dev/null || { echo "adb not found" >&2; exit 1; }
su_do() { adb shell "su -c '$1'" </dev/null | tr -d '\r' || true; }
# with_alarm SECONDS CMD...: the command is killed after SECONDS (macOS has no timeout(1); perl's alarm does)
with_alarm() { perl -e 'alarm shift; exec @ARGV' "$@"; }
su_do_t() { with_alarm "$1" adb shell "su -c '$2'" </dev/null | tr -d '\r' || true; }
[ "$(su_do 'id -u')" = 0 ] || { echo "su does not work on the device" >&2; exit 1; }
mkdir -p "$OUT"
su_do "rm -rf $T; mkdir -p $T" >/dev/null

# dump NAME DEVICE: one block device into OUTDIR/NAME.img.gz, verified
dump() {
    name=$1 dev=$2
    # a rerun resumes: a partition already listed in SHA256SUMS (verified) is skipped
    if grep -q "  $name.img\$" "$OUT/SHA256SUMS" 2>/dev/null; then printf '%-20s already verified\n' "$name"; return; fi
    size=$(su_do "blockdev --getsize64 $dev")
    [ -n "$size" ] || { echo "$name: cannot read size (adb offline?)" >&2; exit 1; }
    pieces=$(( (size + CHUNK * 1048576 - 1) / (CHUNK * 1048576) ))
    printf '%-20s %6s MiB  ' "$name" $((size / 1048576))
    want=$(su_do_t 1800 "sha256sum $dev" | cut -d' ' -f1)
    while ! printf '%s' "$want" | grep -q -E '^[0-9a-f]{64}$'; do
        echo " device sha256 failed, reconnecting" >&2; sleep 5
        case ${ANDROID_SERIAL:-} in *:*) adb disconnect "$ANDROID_SERIAL" >/dev/null 2>&1; adb connect "$ANDROID_SERIAL" >/dev/null 2>&1; sleep 3;; esac
        want=$(su_do_t 1800 "sha256sum $dev" | cut -d' ' -f1)
    done
    # pieces already appended by an interrupted run (sidecar count) are kept; without the sidecar start over
    i=$(cat "$OUT/.$name.pieces" 2>/dev/null || echo 0)
    [ "$i" -gt 0 ] && [ -s "$OUT/$name.img.gz" ] || { : > "$OUT/$name.img.gz"; i=0; }
    [ "$i" -eq 0 ] || echo " resuming at piece $i" >&2
    while [ $i -lt $pieces ]; do
        # one piece: dd+gzip on the device, pull, integrity check; any failure (adb over TCP drops now and
        # then under this load) reconnects and retries the same piece, up to 8 times
        n=0
        until piece_ok=$( { su_do_t 600 "dd if=$dev bs=1048576 skip=$((i * CHUNK)) count=$CHUNK 2>/dev/null | gzip -1 > $T/piece.gz && echo done" ; } 2>/dev/null | tail -1 ) \
              && [ "$piece_ok" = done ] \
              && with_alarm 600 adb pull $T/piece.gz "$OUT/.piece.gz" </dev/null >/dev/null 2>&1 \
              && gzip -t "$OUT/.piece.gz" 2>/dev/null; do
            n=$((n + 1)); [ $n -lt 8 ] || { echo " piece $i failed 8 times" >&2; exit 1; }
            echo " piece $i failed, reconnecting ($n)" >&2; sleep 5
            case ${ANDROID_SERIAL:-} in *:*) adb disconnect "$ANDROID_SERIAL" >/dev/null 2>&1; adb connect "$ANDROID_SERIAL" >/dev/null 2>&1; sleep 3;; esac
        done
        cat "$OUT/.piece.gz" >> "$OUT/$name.img.gz"
        i=$((i + 1))
        echo "$i" > "$OUT/.$name.pieces"
        printf '.'
    done
    rm -f "$OUT/.piece.gz"
    have=$(gunzip -c "$OUT/$name.img.gz" | { shasum -a 256 2>/dev/null || sha256sum; } | cut -d' ' -f1)
    rm -f "$OUT/.$name.pieces"
    if [ "$have" = "$want" ]; then
        echo "$want  $name.img" >> "$OUT/SHA256SUMS"
        echo " ok"
    else
        echo " CHECKSUM MISMATCH (device $want, copy $have)"
        echo "$name" >> "$OUT/FAILED"
    fi
}

touch "$OUT/SHA256SUMS"; rm -f "$OUT/FAILED"
su_do 'getprop' > "$OUT/getprop.txt"
su_do 'cat /proc/partitions; ls -l /dev/block/by-name/' > "$OUT/partitions.txt"
echo "==> partitions (by name), eMMC boot areas, GPT"
# name -> target, whole disk excluded, each target once (the first name wins; by-name lists spl_a before mmcblk0boot0)
su_do 'ls -l /dev/block/by-name/' | awk '$NF ~ /\/dev\/block\// && $NF != "/dev/block/mmcblk0" {print $8, $NF}' \
  | sort -k1,1 | awk '!seen[$2]++' > "$OUT/.names"
while read -r n dev; do dump "$n" "$dev"; done < "$OUT/.names"
rm -f "$OUT/.names"
disk=$(su_do 'cat /sys/block/mmcblk0/size')
su_do "dd if=/dev/block/mmcblk0 bs=512 count=34 2>/dev/null > $T/gpt.head; dd if=/dev/block/mmcblk0 bs=512 skip=$((disk - 33)) count=33 2>/dev/null > $T/gpt.tail" >/dev/null
adb pull $T/gpt.head "$OUT/gpt-primary.bin" </dev/null >/dev/null 2>&1
adb pull $T/gpt.tail "$OUT/gpt-backup.bin" </dev/null >/dev/null 2>&1
# the space after the last partition (where a Linux region goes): empty or not, 16 samples of 1 MiB
last=$(su_do 'e=0; for p in /sys/block/mmcblk0/mmcblk0p*; do x=$(( $(cat $p/start) + $(cat $p/size) )); [ $x -gt $e ] && e=$x; done; echo $e')
step=$(( (disk - last) / 2048 / 16 ))
# less than 16 MiB behind the last partition (userdata fills the disk): nothing to sample, and a dd that starts
# past the end of the eMMC spins in the kernel instead of failing (mu300-linux FINDINGS 10, seen here 2026-10-29)
if [ "$step" -eq 0 ]; then nz=0; else
nz=$(su_do "n=0; i=0; while [ \$i -lt 16 ]; do c=\$(dd if=/dev/block/mmcblk0 bs=1048576 skip=\$(( $last / 2048 + i * $step )) count=1 2>/dev/null | tr -d \"\\000\" | wc -c); [ \$c -gt 0 ] && n=\$((n + 1)); i=\$((i + 1)); done; echo \$n"); fi
echo "unpartitioned space after sector $last: $(( (disk - last) / 2048 )) MiB, $nz of 16 samples non-zero" | tee "$OUT/unpartitioned.txt"
su_do "rm -rf $T" >/dev/null
echo
if [ -e "$OUT/FAILED" ]; then echo "FAILED: $(tr '\n' ' ' < "$OUT/FAILED")"; exit 1; fi
echo "all $(wc -l < "$OUT/SHA256SUMS" | tr -d ' ') images verified; $(du -sh "$OUT" | cut -f1) in $OUT"
