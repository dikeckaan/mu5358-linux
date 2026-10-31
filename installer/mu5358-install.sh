#!/system/bin/sh
# MU5358 Linux installer, device side: what a rooted Android (Magisk) on the ZTE U30 Pro does to install, start and
# remove Linux. The web installer (site/web-installer) pushes this file, android-files.txt and a release's files to
# /data/local/tmp/mu5358-web and runs, as root,
#
#   sh mu5358-install.sh COMMAND [ARG...]
#
#   status               MU5358-STATUS {json}: the device, root, misc, slot b, fulldumpdb, what is installed, a job.
#                        Writes nothing.
#   take SUMS_SHA256     the pushed files (in/) moved to root's directory (/data/adb/mu5358/in) and checked there:
#                        SHA256SUMS against SUMS_SHA256 (the page has it from GitHub), every file against SHA256SUMS.
#                        in/lang (optional: en, tr, zh) is the screen's language for the system
#   password             one line on stdin: root's password for the system as a sha512-crypt hash ($6$...), made by
#                        the page (the device never sees the password). Needed before run install: SSH and the
#                        panel never open without one. in/authorized_keys (optional) is root's SSH keys.
#   run install [region [MIB] | fulldumpdb]   start the installation as a job: it goes on if the page goes (a closed
#                        tab, a computer that sleeps). MU5358-JOB started. region (the default): the system behind
#                        Android's userdata; where there is no such region yet, the job makes one of MIB (default
#                        4096) by shrinking userdata's f2fs while it is mounted (nothing of Android's is erased) and
#                        moving userdata's end in the GPT, and ends with MU5358-REBOOT-NEEDED (exit 3): Android
#                        must start once with the new table, then the same run install goes on. fulldumpdb: the
#                        system in the 2 GiB vendor dump partition, nothing repartitioned
#   run update           a newer release over an installed system, from Android, nothing else changed: the system
#                        written again into the same place with the device's settings carried over (/etc/config,
#                        the passwords and keys, the panel's SMS pool and traffic ledger, the screen's settings, the
#                        web server's own key), then boot_b and init_boot_b; misc untouched (boot-linux tries it).
#                        A downgrade is refused unless MU5358_ALLOW_DOWNGRADE=1.
#   run uninstall [keep-space]   the job that removes it; the region's room goes back to userdata (Android's vold
#                        grows the f2fs at its next start) unless keep-space
#   follow               the job's output from its start, as it comes; ends with MU5358-EXIT <status>
#   boot-linux [dry]     arm slot b for one boot (32 bytes of misc, read back) and reboot; dry: arm nothing
#   backup               MU5358-FILE lines: the copies of what an installation wrote over (taken by the first one)
#   cat NAME             one of them, base64, for the page to keep
#
# What it writes, and in what order (install): the backup of boot_b, init_boot_b, misc and both GPT copies first;
# the room (region, once); slot b's copy of Android's boot chain (only partitions that differ from slot a's, each
# read back); the system (an ext4, label mu5358root, plus this device's own vendor files); then boot_b (the release's kernel in a copy of
# boot_a) and init_boot_b (the release's ramdisk in a copy of init_boot_a). misc is not touched: Linux starts only
# when boot-linux is asked for, once, and the board goes back to Android at the next reboot whatever happens.
# Never: slot a, the NV partitions (nr_*nv*, prodnv, calinv, miscdata, persist, ztepersist), any GPT entry but
# userdata's end, any of userdata's data (the kernel moves its f2fs out of the tail before the end moves).
#
# Lines for the page start with MU5358-; every other line is for people.
set -u
[ "$(id -u)" = 0 ] || { echo "MU5358-ERROR not-root"; exit 2; }

# Magisk's busybox for every tool (the same applets on every device: toybox differs between builds)
BB=${MU5358_BB:-/data/adb/magisk/busybox}
if [ -z "${MU5358_IN_BB:-}" ] && [ -x "$BB" ]; then
    MU5358_IN_BB=1 ASH_STANDALONE=1 exec "$BB" sh "$0" "$@"
fi

HOME_DIR=/data/adb/mu5358
IN=$HOME_DIR/in
PUSHED=/data/local/tmp/mu5358-web/in
BACKUP=$HOME_DIR/backup
JOB=$HOME_DIR/job
BYNAME=/dev/block/by-name
MNT=$HOME_DIR/mnt
HERE=$(cd "$(dirname "$0")" && pwd)
# Slot b's boot chain: what the bench unit has had since 2026-10-29 (docs/GKI-SYSTEM.md). Slot a's copy of each,
# never an NV partition (nr_deltanv_b, nr_downloadnv_b stay as they are).
CHAIN="boot init_boot vendor_boot dtbo vbmeta vbmeta_system vbmeta_vendor vbmeta_system_ext vbmeta_product vbmeta_odm
uboot sml trustos teecfg pm_sys ch_sys l_agdsp nr_modem nr_phy nr_phy_npi logo fbootlogo"
# the release's files
F_SUMS=SHA256SUMS F_KERNEL=mu5358-kernel.Image.gz F_RAMDISK=mu5358-ramdisk.tar.gz F_ROOT=mu5358-openwrt.img.gz
F_TOOLS=mu5358-tools.tar.gz   # gpt-userdata, f2fs-shrink (installer/, static)
ROOT_MIB=2047          # the image's filesystem: fulldumpdb less its last MiB (boot/init's persistent log)
DISK=/dev/block/mmcblk0
MIN_ANDROID_FREE=$((4 * 1024 * 1024 * 1024))   # what /data keeps free after a shrink
# the vendor modules the system loads from /lib/mu5358/modules (openwrt/customize.sh MODS), the GPU's, the
# ramdisk's (sprd_pmic_wdt feeds the PMIC watchdog: without it the board resets at ~280 s)
MODS="sprd_cpu_cooling sprd_shell_thm sprd_virt_thm sprd_freq_limit sprd_ddr_dvfs dmc_drv sprd_bcl
sprd_ump96xx_tsensor thermal-generic-adc zsmalloc zram unisoc_pnp unisoc_scufreq cpufreq_userspace unisoc_last_kmsg
zte_bootreason cfg80211 aic8800_bsp aic8800_fdrv mali_kbase"
RD_MODS=sprd_pmic_wdt

say() { echo "$*"; }
step() { echo "MU5358-STEP $1"; }
die() { echo "MU5358-ERROR $*"; exit 1; }
js() { printf %s "$1" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr -d '\n\r'; }
fsize() { stat -c %s "$1"; }
sha() { sha256sum "$1" | cut -c1-64; }
psize() { blockdev --getsize64 "$1" 2>/dev/null || stat -c %s "$1"; }
prop() { getprop "$1" 2>/dev/null; }

# ---- bytes -----------------------------------------------------------------------------------------------------
u32() { dd if="$1" bs=1 skip="$2" count=4 2>/dev/null | od -An -tu4 | tr -d ' '; }    # little endian (arm64)
u64be() { dd if="$1" bs=1 skip="$2" count=8 2>/dev/null | od -An -tx1 | tr -d ' \n' | sed 's/^/0x/' | xargs printf %d; }
bytes() { while [ $# -gt 0 ]; do printf "\\$(printf %o "$1")"; shift; done; }          # bytes N...: octal escapes
le32() { bytes $(($1 & 255)) $(($1 >> 8 & 255)) $(($1 >> 16 & 255)) $(($1 >> 24 & 255)); }
be64() { bytes $(($1 >> 56 & 255)) $(($1 >> 48 & 255)) $(($1 >> 40 & 255)) $(($1 >> 32 & 255)) \
    $(($1 >> 24 & 255)) $(($1 >> 16 & 255)) $(($1 >> 8 & 255)) $(($1 & 255)); }
hex() { od -An -tx1 -v | tr -d ' \n'; }
unhex() { _h=$1; while [ -n "$_h" ]; do bytes "0x${_h%"${_h#??}"}"; _h=${_h#??}; done; }
put() { dd of="$1" bs=1 seek="$2" conv=notrunc 2>/dev/null; }                          # put FILE OFFSET < bytes
# CRC-32 of the bytes on stdin, little endian, as hex: gzip ends every stream with it
crc32le() { gzip -c | tail -c 8 | head -c 4 | hex; }

# bootimg_put STOCK PAYLOAD kernel|ramdisk OUT: the v4 boot image STOCK with its kernel or its ramdisk replaced by
# PAYLOAD, the other one absent, its AVB data (vbmeta blob, footer) moved behind it - boot/build-initboot.py's
# replace_payload, byte for byte (Magisk does the same with the image this device boots today)
bootimg_put() {
    _st=$1 _pl=$2 _w=$3 _o=$4
    [ "$(head -c 8 "$_st")" = 'ANDROID!' ] && [ "$(u32 "$_st" 40)" = 4 ] || { say "$_st: not a v4 boot image"; return 1; }
    _ks=$(u32 "$_st" 8) _rs=$(u32 "$_st" 12)
    case $_w in kernel) [ "$_rs" = 0 ] ;; ramdisk) [ "$_ks" = 0 ] ;; *) false ;; esac ||
        { say "$_st: has more than a $_w"; return 1; }
    _t=$(fsize "$_st")
    [ $((_t % 4096)) = 0 ] && [ "$(tail -c 64 "$_st" | head -c 4)" = AVBf ] || { say "$_st: no AVB footer"; return 1; }
    _vo=$(u64be "$_st" $((_t - 64 + 20))) _vs=$(u64be "$_st" $((_t - 64 + 28)))
    _pn=$(fsize "$_pl")
    _end=$((4096 + (_pn + 4095) / 4096 * 4096))
    [ $((_end + _vs)) -le $((_t - 4096)) ] || { say "$_pl: too large for the partition"; return 1; }
    rm -f "$_o"
    dd if=/dev/zero of="$_o" bs=4096 count=$((_t / 4096)) 2>/dev/null &&
        dd if="$_st" of="$_o" bs=4096 count=1 conv=notrunc 2>/dev/null &&
        le32 "$_pn" | put "$_o" $([ "$_w" = kernel ] && echo 8 || echo 12) &&
        dd if="$_pl" of="$_o" bs=4096 seek=1 conv=notrunc 2>/dev/null &&
        dd if="$_st" bs=1 skip="$_vo" count="$_vs" 2>/dev/null | put "$_o" "$_end" &&
        { tail -c 64 "$_st" | head -c 12; be64 "$_end"; be64 "$_end"; be64 "$_vs"; tail -c 28 "$_st"; } |
            put "$_o" $((_t - 64)) &&
        [ "$(fsize "$_o")" = "$_t" ]
}

# ---- misc ------------------------------------------------------------------------------------------------------
# bootloader_control at 0x800 of misc: 32 bytes, magic BCAB at 4, slot bytes at 12 (a) and 14 (b): priority (0-3) |
# tries (4-6) | successful (7), CRC-32 of the first 28 at 28
bc_live() { dd if=$BYNAME/misc bs=1 skip=2048 count=32 2>/dev/null | hex; }
bc_valid() {
    [ ${#1} = 64 ] && [ "$(printf %s "$1" | cut -c9-16)" = 42434142 ] &&
        [ "$(unhex "$(printf %s "$1" | cut -c1-56)" | crc32le)" = "$(printf %s "$1" | cut -c57-64)" ]
}
# bc_state HEX: android (a current, priority 15, successful), trial (b to be tried once), other
bc_state() {
    bc_valid "$1" || { echo invalid; return; }
    _s=$(printf %s "$1" | cut -c1-4) _a=$(printf %s "$1" | cut -c25-26) _b=$(printf %s "$1" | cut -c29-30)
    if [ "$_s" = 5f61 ] && [ $((0x$_a & 0x8f)) = $((0x8f)) ]; then echo android
    elif [ "$_s" = 5f62 ] && [ "$_b" = 2f ]; then echo trial
    else echo other; fi
}
# bc_trial ANDROID_HEX: build-initboot.py's trial block - b priority 15 with 2 tries (Unisoc LK takes tries 1 and not
# successful as a boot that already failed), a at 14 and still successful
bc_trial() {
    _a=$(printf %s "$1" | cut -c25-26)
    _x="5f620000$(printf %s "$1" | cut -c9-24)$(printf %02x $(( (0x$_a & 0xf0) | 14 )))$(printf %s "$1" | cut -c27-28)2f$(printf %s "$1" | cut -c31-56)"
    printf %s%s "$_x" "$(unhex "$_x" | crc32le)"
}
bc_write() {  # bc_write HEX: the block, read back
    unhex "$1" > $HOME_DIR/bc.bin && [ "$(fsize $HOME_DIR/bc.bin)" = 32 ] || return 1
    dd if=$HOME_DIR/bc.bin of=$BYNAME/misc bs=1 seek=2048 count=32 conv=notrunc,fsync 2>/dev/null
    sync
    echo 3 > /proc/sys/vm/drop_caches
    [ "$(bc_live)" = "$1" ]
}

# ---- partitions ------------------------------------------------------------------------------------------------
# zero DEV [BYTES]: whether the first BYTES (default all) of DEV are zeros. Fails closed: a read that gives fewer
# bytes than asked (an error, a device that answers nothing) is not "zeros" (mu300-linux FINDINGS: probes fail closed)
zero() {
    _n=${2:-$(psize "$1")}
    [ -n "$_n" ] && [ "$_n" -gt 0 ] || return 1
    [ "$(head -c "$_n" "$1" 2>/dev/null | wc -c)" = "$_n" ] || return 1
    [ "$(head -c "$_n" "$1" | tr -d '\000' | head -c 1 | wc -c)" = 0 ]
}
label() { dd if="$1" bs=1 skip=1144 count=10 2>/dev/null; }
# fulldumpdb: empty (all zeros: Android's full RAM dump, which this firmware never writes), ours, used
fd_state() {
    _d=$BYNAME/fulldumpdb
    [ -b $_d ] || { echo absent; return; }
    [ "$(label $_d)" = mu5358root ] && { echo ours; return; }
    [ "$(head -c 1048576 $_d 2>/dev/null | wc -c)" = 1048576 ] || { echo unreadable; return; }
    zero $_d && echo empty || echo used
}
# write_part IMG DEV: IMG onto the start of DEV, read back
write_part() {
    _n=$(fsize "$1")
    [ "$_n" -le "$(psize "$2")" ] || { say "$1 is larger than $2"; return 1; }
    dd if="$1" of="$2" bs=1048576 conv=fsync 2>/dev/null; sync
    echo 3 > /proc/sys/vm/drop_caches
    [ "$(head -c "$_n" "$2" | sha256sum | cut -c1-64)" = "$(sha "$1")" ] || { say "$2: read back differs"; return 1; }
}

# ---- the region behind userdata ---------------------------------------------------------------------------------
# region_info: UD_START UD_END (userdata, inclusive) LAST (the last usable LBA), R0 (the region's first sector: the
# first 2 MiB boundary behind userdata, boot/init's find_home), RLOG (its last MiB, boot/init's persistent log),
# RSIZE (bytes up to the log), RSTATE: ours (an ext4 labelled mu5358root at R0), free (room, no system), absent
region_info() {
    set -- $($IN/gpt-userdata $DISK 2>/dev/null || $HOME_DIR/gpt-userdata $DISK 2>/dev/null)
    [ "${1:-}" = userdata ] || return 1
    UD_START=$2 UD_END=$3 LAST=$5
    R0=$(( (UD_END + 1 + 4095) / 4096 * 4096 )) RLOG=$((LAST + 1 - 2048))
    RSIZE=$(( (RLOG - R0) * 512 ))
    if [ $RSIZE -lt $((ROOT_MIB * 1048576)) ]; then RSTATE=absent; RSIZE=0
    elif [ "$(dd if=$DISK bs=512 skip=$R0 count=4 2>/dev/null | tail -c +1145 | head -c 10)" = mu5358root ]; then RSTATE=ours
    else RSTATE=free; fi
}
# the kernel's userdata: the size it has now (the table it read at boot)
ud_kernel_sectors() { cat /sys/class/block/$(basename "$(readlink -f $BYNAME/userdata)")/size; }

installed() { [ -f $HOME_DIR/installed ] && sed -n "s/^$1=//p" $HOME_DIR/installed; }

# ---- status ----------------------------------------------------------------------------------------------------
status() {
    _bc=$(bc_live)
    _bstate=zero
    if [ -b $BYNAME/boot_b ]; then
        if zero $BYNAME/boot_b 1048576; then _bstate=empty
        elif [ -n "$(installed boot_b)" ] && [ "$(head -c 1048576 $BYNAME/boot_b | sha256sum | cut -c1-64)" = "$(installed boot_b_head)" ]; then _bstate=linux
        elif cmp -s -n 1048576 $BYNAME/boot_a $BYNAME/boot_b 2>/dev/null; then _bstate=android
        else _bstate=other; fi
    fi
    _job=none
    if [ -f $JOB/pid ] && kill -0 "$(cat $JOB/pid)" 2>/dev/null; then _job=running
    elif [ -f $JOB/exit ]; then _job="exit-$(cat $JOB/exit)"; fi
    printf 'MU5358-STATUS {"model":"%s","device":"%s","platform":"%s","serial":"%s","android":"%s","build":"%s"' \
        "$(js "$(prop ro.product.model)")" "$(js "$(prop ro.product.device)")" "$(js "$(prop ro.board.platform)")" \
        "$(js "$(prop ro.serialno)")" "$(js "$(prop ro.build.version.release)")" "$(js "$(prop ro.build.display.id)")"
    printf ',"slot":"%s","magisk":"%s","magiskboot":%s,"misc":"%s","boot_b":"%s","fulldumpdb":"%s","fulldumpdb_size":%s' \
        "$(js "$(prop ro.boot.slot_suffix)")" "$(js "$(magisk -v 2>/dev/null | head -n1)")" \
        "$([ -x /data/adb/magisk/magiskboot ] && echo true || echo false)" "$(bc_state "$_bc")" "$_bstate" \
        "$(fd_state)" "$(psize $BYNAME/fulldumpdb 2>/dev/null || echo 0)"
    if region_info; then
        printf ',"region":{"state":"%s","start":%s,"size":%s},"userdata":{"start":%s,"size":%s,"kernel_size":%s}' \
            $RSTATE $R0 $RSIZE $UD_START $(( (UD_END + 1 - UD_START) * 512 )) $(( $(ud_kernel_sectors) * 512 ))
    fi
    printf ',"root":"%s"' "$(installed target)"
    printf ',"installed":"%s","backup":%s,"job":"%s","free_data":%s}\n' "$(js "$(installed version)")" \
        "$([ -f $BACKUP/SHA256SUMS ] && echo true || echo false)" "$_job" \
        "$(df -k /data | awk 'NR == 2 { print $4 * 1024 }')"
}

# ---- take ------------------------------------------------------------------------------------------------------
# The pushed files are the adb user's: moved (a rename takes the name, follows no link) into root's directory, and
# only looked at there.
take() {
    _want=${1:?take SUMS_SHA256}
    mkdir -p $IN && chmod 700 $HOME_DIR $IN
    rm -f $IN/*
    for _f in $F_SUMS $F_KERNEL $F_RAMDISK $F_ROOT $F_TOOLS; do
        [ -f "$PUSHED/$_f" ] && [ ! -L "$PUSHED/$_f" ] || die "missing $_f"
        mv "$PUSHED/$_f" "$IN/$_f" || die "cannot take $_f"
    done
    [ "$(sha $IN/$F_SUMS)" = "$_want" ] || die "SHA256SUMS is not the release's"
    for _f in $F_KERNEL $F_RAMDISK $F_ROOT $F_TOOLS; do
        _s=$(awk -v f="$_f" '$2 == f || $2 == "*" f { print $1 }' $IN/$F_SUMS)
        [ -n "$_s" ] || die "$_f not in SHA256SUMS"
        [ "$(sha "$IN/$_f")" = "$_s" ] || die "$_f: checksum differs"
        echo "MU5358-INFO $_f ok"
    done
    tar -xzf $IN/$F_TOOLS -C $IN gpt-userdata f2fs-shrink && chmod 700 $IN/gpt-userdata $IN/f2fs-shrink ||
        die "$F_TOOLS"
    cp $IN/gpt-userdata $HOME_DIR/gpt-userdata
    sed -n 's/^# version //p' $IN/$F_SUMS | head -n1 > $IN/version
    # the screen's language, when the page sends one (in/lang: en, tr or zh)
    rm -f $IN/lang
    [ -f "$PUSHED/lang" ] && [ ! -L "$PUSHED/lang" ] && head -c 2 "$PUSHED/lang" | tr -cd 'a-z' > $IN/lang
    rm -f "$PUSHED/lang"
    rm -f $IN/authorized_keys
    if [ -f "$PUSHED/authorized_keys" ] && [ ! -L "$PUSHED/authorized_keys" ]; then
        head -c 8192 "$PUSHED/authorized_keys" | grep -E '^(ssh-(ed25519|rsa)|ecdsa-sha2-nistp[0-9]+) [A-Za-z0-9+/=]+' > $IN/authorized_keys
        chmod 600 $IN/authorized_keys
    fi
    rm -f "$PUSHED/authorized_keys"
    echo "MU5358-TAKEN $(cat $IN/version)"
}

password() {
    _h=
    read -r _h
    printf %s "$_h" | grep -qE '^\$6\$[./A-Za-z0-9]{1,16}\$[./A-Za-z0-9]{86}$' || die "not a sha512-crypt hash"
    mkdir -p $IN && chmod 700 $HOME_DIR $IN
    (umask 077; printf '%s\n' "$_h" > $IN/shadow-hash)
    echo "MU5358-PASSWORD set"
}

# ---- install ---------------------------------------------------------------------------------------------------
check() {
    step check
    case "$(prop ro.board.platform)$(prop ro.product.device)$(prop ro.product.model)" in
        *ums9632*|*MU5358*|*U30*Pro*) ;;
        *) die "not a ZTE U30 Pro (MU5358, UMS9632): $(prop ro.product.model)" ;;
    esac
    [ "$(prop ro.boot.slot_suffix)" = _a ] || die "Android does not run from slot a"
    [ -x /data/adb/magisk/magiskboot ] || die "no magiskboot (Magisk)"
    for _p in misc fulldumpdb $(for _c in $CHAIN; do echo ${_c}_a ${_c}_b; done); do
        [ -b $BYNAME/$_p ] || die "no partition $_p"
    done
    _bc=$(bc_live)
    [ "$(bc_state "$_bc")" = android ] || die "misc is not Android's on slot a ($(bc_state "$_bc"))"
    for _f in $F_KERNEL $F_RAMDISK $F_ROOT gpt-userdata f2fs-shrink; do [ -s "$IN/$_f" ] || die "take the release first"; done
    region_info || die "cannot read the GPT (userdata must be its last partition)"
    if [ "$TARGET" = fulldumpdb ]; then
        case $(fd_state) in empty|ours) ;; *) die "fulldumpdb is in use (not zeros, not a MU5358 Linux)" ;; esac
        [ "$(psize $BYNAME/fulldumpdb)" -ge $(((ROOT_MIB + 1) * 1048576)) ] || die "fulldumpdb is too small"
    else
        # a system in fulldumpdb would win at boot (boot/init looks there first): only one of the two
        case $(fd_state) in empty|ours) ;; *) die "fulldumpdb is in use (not zeros, not a MU5358 Linux)" ;; esac
    fi
    _free=$(df -k /data | awk 'NR == 2 { print $4 }')
    [ "$_free" -ge $((600 * 1024)) ] || die "less than 600 MiB free on /data"
    [ -s $IN/shadow-hash ] || [ "${MU5358_NO_PASSWORD:-}" = 1 ] || die "no password set"
    echo "$_bc" > $HOME_DIR/bc-android.hex
}

backup() {
    step backup
    # the first installation's copies stay: they are what the device had before MU5358 Linux
    [ -f $BACKUP/SHA256SUMS ] && { say "kept from $(cat $BACKUP/date)"; return 0; }
    rm -rf $BACKUP.new && mkdir -p $BACKUP.new && chmod 700 $BACKUP.new
    dd if=$BYNAME/boot_b of=$BACKUP.new/boot_b.img bs=1048576 2>/dev/null &&
        dd if=$BYNAME/init_boot_b of=$BACKUP.new/init_boot_b.img bs=1048576 2>/dev/null &&
        dd if=$BYNAME/misc of=$BACKUP.new/misc-4k.bin bs=4096 count=1 2>/dev/null &&
        dd if=$DISK of=$BACKUP.new/gpt-primary.bin bs=512 count=34 2>/dev/null &&
        dd if=$DISK of=$BACKUP.new/gpt-backup.bin bs=512 skip=$((LAST + 1)) count=33 2>/dev/null || die "backup failed"
    # empty partitions compress to nothing: the page can download them
    for _f in boot_b.img init_boot_b.img; do gzip -1 $BACKUP.new/$_f || die "backup failed"; done
    (cd $BACKUP.new && sha256sum *.gz *.bin > SHA256SUMS) && date -u +%FT%TZ > $BACKUP.new/date &&
        mv $BACKUP.new $BACKUP || die "backup failed"
}

# space: the region, made where there is none (TARGET region)
space() {
    step space
    [ "$TARGET" = region ] || return 0
    _ksz=$(ud_kernel_sectors)
    if [ $((UD_END + 1 - UD_START)) != "$_ksz" ]; then
        # the table was changed in this Android's boot: the kernel still has the old userdata over the region
        echo "MU5358-REBOOT-NEEDED"; say "the new partition table takes effect at the next start of Android"; exit 3
    fi
    case ${MU5358_FORCE_SHRINK:-}$RSTATE in
        ours|free) say "the region: sectors $R0..$((RLOG - 1)), $((RSIZE / 1048576)) MiB ($RSTATE)"; return 0 ;;
    esac
    _mib=${LINUX_MIB:-4096}
    [ "$_mib" -ge 2560 ] || die "the system needs at least 2560 MiB"
    # the new end: the region's 2 MiB boundary at the size asked for from the end of the usable area
    _r0=$(( (LAST + 1 - _mib * 2048) / 4096 * 4096 ))
    _end=$((_r0 - 1))
    _secs=$((_end + 1 - UD_START))
    # userdata's f2fs: on the dm device /data is mounted from, its superblock at 1 KiB
    _dm=$(awk '$2 == "/data" && $3 == "f2fs" { print $1; exit }' /proc/mounts)
    [ -b "$_dm" ] || die "/data is not a mounted f2fs"
    [ "$(dd if=$_dm bs=1 skip=1024 count=4 2>/dev/null | hex)" = 1020f5f2 ] || die "no f2fs superblock on $_dm"
    _lbps=$(u32 $_dm $((1024 + 20))) _sps=$(u32 $_dm $((1024 + 24)))   # log_blocks_per_seg, segs_per_sec
    _bps=$(( (1 << _lbps) * _sps ))
    [ "$_bps" -ge 512 ] || die "f2fs: unexpected section size ($_bps blocks)"
    _old=$(dd if=$_dm bs=1 skip=$((1024 + 36)) count=8 2>/dev/null | od -An -tu8 | tr -d ' ')
    _new=$(( _secs / 8 / _bps * _bps ))
    [ "$_new" -lt "$_old" ] || die "userdata is already small enough"
    _free=$(df -k /data | awk 'NR == 2 { print $4 * 1024 }')
    [ $((_free - (_old - _new) * 4096)) -ge $MIN_ANDROID_FREE ] ||
        die "too little free space on /data: $((_free / 1048576)) MiB free, the shrink takes $(( (_old - _new) * 4 / 1024 )) MiB"
    say "userdata: f2fs $_old -> $_new blocks of 4 KiB, end sector $UD_END -> $_end; Linux gets $(( (LAST + 1 - _r0) / 2048 )) MiB"
    $IN/gpt-userdata $DISK $_end --dry-run || die "the GPT would not take the new end"
    [ "${MU5358_DRY:-}" = 1 ] && { say "dry run: nothing changed"; exit 0; }
    # the kernel checks the free space and does a dry run itself before it moves anything
    sync
    $IN/f2fs-shrink /data $_new || die "the f2fs shrink was refused: nothing changed"
    # vold grows the f2fs back to the partition at every start of Android: the table goes in the same boot
    $IN/gpt-userdata $DISK $_end || die "the GPT write failed: Android grows its f2fs back at its next start"
    echo "MU5358-REBOOT-NEEDED"
    say "room made; Android must start once with the new table, then run the installation again"
    exit 3
}

chain() {
    step chain
    for _c in $CHAIN; do
        [ "$(psize $BYNAME/${_c}_a)" = "$(psize $BYNAME/${_c}_b)" ] || die "${_c}: the slots differ in size"
        if cmp -s $BYNAME/${_c}_a $BYNAME/${_c}_b; then continue; fi
        # boot and init_boot get Linux's images below; copied all the same, so that a stop in between leaves a slot
        # b that is Android's
        say "copy ${_c}_a to ${_c}_b"
        dd if=$BYNAME/${_c}_a of=$BYNAME/${_c}_b bs=1048576 conv=fsync 2>/dev/null; sync
        echo 3 > /proc/sys/vm/drop_caches
        cmp -s $BYNAME/${_c}_a $BYNAME/${_c}_b || die "${_c}_b: read back differs"
    done
}

# the vendor files of this device, into the system mounted at $1 (openwrt/customize.sh's, from Android's mounts)
vendor_files() {
    _r=$1
    mkdir -p $_r/lib/mu5358/modules $_r/odm/firmware/aic8800 $_r/mnt/vendor $_r/etc/mu5358 || return 1
    for _m in $MODS; do
        cp /vendor_dlkm/lib/modules/$_m.ko $_r/lib/mu5358/modules/ || { say "no $_m.ko in vendor_dlkm"; return 1; }
    done
    cp /odm/firmware/aic8800/* $_r/odm/firmware/aic8800/ || { say "no AIC8800 firmware in /odm"; return 1; }
    # the board's Wi-Fi calibration and MAC (prodnv, as Android mounts it; read only)
    [ -f /mnt/vendor/aic_userconfig_8800d80x2.txt ] && cp /mnt/vendor/aic_userconfig_8800d80x2.txt $_r/mnt/vendor/
    [ -f /mnt/vendor/wifimac.txt ] && grep -q -E '^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$' /mnt/vendor/wifimac.txt &&
        cp /mnt/vendor/wifimac.txt $_r/etc/mu5358/wifimac
    # the modem chroot and the GPU's userspace (modem/README.md): root's, directories and programs 0755, the
    # property files 0444, the rest 0644
    _a=$_r/opt/mu5358/android
    # what an earlier installation put there goes; the release's own opt/ (gltest, gpu/build.sh) stays
    mkdir -p $_a && find $_a -mindepth 1 -maxdepth 1 ! -name opt -exec rm -rf {} + || return 1
    (cd / && tar -chf - $(grep -v '^#' "$HERE/android-files.txt" | while read -r _p; do [ -e "/$_p" ] && echo "$_p"; done) \
        dev/__properties__ 2>/dev/null) | tar -xf - -C $_a || return 1
    [ -s $_a/vendor/bin/modem_control ] && [ -d $_a/dev/__properties__ ] || { say "no modem_control"; return 1; }
    mv $_a/dev/__properties__ $_a/dev-properties && rmdir $_a/dev
    mkdir -p $_a/system/bin $_a/linkerconfig
    ln -sfn /apex/com.android.runtime/bin/linker64 $_a/system/bin/linker64
    : > $_a/linkerconfig/ld.config.txt
    [ -f $_a/vendor/lib64/egl/libGLES_mali.so ] && ln -sfn egl/libGLES_mali.so $_a/vendor/lib64/libOpenCL.so
    # persist.log.tag S (ZTE's) silences the daemons' logs: V in the chroot's copy (modem/patch-props.py)
    for _f in $_a/dev-properties/*; do
        _o=$(grep -obUa 'persist\.log\.tag' "$_f" 2>/dev/null | head -n1 | cut -d: -f1)
        [ -n "$_o" ] && [ "$_o" -ge 96 ] || continue
        [ "$(dd if="$_f" bs=1 skip=$((_o - 92)) count=2 2>/dev/null | hex)" = 5300 ] && printf V | put "$_f" $((_o - 92))
    done
    [ -s $IN/lang ] && { mkdir -p $_r/etc/mu5358; cp $IN/lang $_r/etc/mu5358/lang; }
    # root's password: the hash into the system's shadow (its own line replaced), root's SSH keys
    if [ -s $IN/shadow-hash ]; then
        _h=$(cat $IN/shadow-hash)
        awk -v h="$_h" -F: 'BEGIN { OFS = ":" } $1 == "root" { $2 = h } { print }' $_r/etc/shadow > $_r/etc/shadow.new &&
            grep -qF 'root:$6$' $_r/etc/shadow.new && mv $_r/etc/shadow.new $_r/etc/shadow || { say "shadow"; return 1; }
        chmod 600 $_r/etc/shadow
    fi
    if [ -s $IN/authorized_keys ]; then
        mkdir -p $_r/etc/dropbear && cp $IN/authorized_keys $_r/etc/dropbear/authorized_keys &&
            chmod 700 $_r/etc/dropbear && chmod 600 $_r/etc/dropbear/authorized_keys
    fi
    chown -R 0:0 $_a
    find $_a -type d -exec chmod 755 {} +
    find $_a -type f -path '*/bin/*' -exec chmod 755 {} +
    find $_a -type f ! -path '*/bin/*' -exec chmod 644 {} +
    find $_a/dev-properties -type f -exec chmod 444 {} +
    # the release's programs in opt/ (gltest)
    find $_a/opt -type f -exec chmod 755 {} + 2>/dev/null
    du -sh $_a | cut -f1 | sed 's/^/vendor files: /'
}

# What an update carries from the installed system to the new one: the configuration and what the user made, never
# the new release's own files (init scripts, uci-defaults, the version and module lists)
CARRY="etc/config etc/shadow etc/dropbear etc/uhttpd.crt etc/uhttpd.key etc/u30ui.conf etc/u30ui2.conf etc/mu5358/lang
etc/mu5358/ui etc/mu5358/hotspot.conf etc/mu5358/wifi-configured etc/mu5358/sms-pool etc/mu5358/traffic etc/mu5358/adb
etc/mu5358/usb-net etc/mu5358/zram.conf etc/mu5358/user-packages etc/mu5358/charge-limit root/.ssh"
# tag_cmp A B: "newer" when tag A (vYYYY.MM.DD[.N]) is newer than B, "older", or "same"; no sort -V here (Magisk's
# busybox has none)
tag_cmp() {
    _i=1
    while [ $_i -le 4 ]; do
        _x=$(printf %s "${1#v}" | cut -d. -f$_i); _y=$(printf %s "${2#v}" | cut -d. -f$_i)
        _x=${_x:-0}; _y=${_y:-0}
        [ "$_x" -gt "$_y" ] 2>/dev/null && { echo newer; return; }
        [ "$_x" -lt "$_y" ] 2>/dev/null && { echo older; return; }
        _i=$((_i + 1))
    done
    echo same
}
carry_save() {   # the installed system's files into $HOME_DIR/carry.tar, from the region (fulldumpdb: as installed)
    step carry
    rm -f $HOME_DIR/carry.tar
    if [ "$TARGET" = region ]; then
        _lo=$(/system/bin/losetup -f -s -r -o $((R0 * 512)) $DISK 2>&1) && [ -b "$_lo" ] || die "no loop device: $_lo"
        _src=$_lo
    else
        _src=$BYNAME/fulldumpdb
    fi
    mkdir -p $MNT && umount $MNT 2>/dev/null
    mount -t ext4 -o ro,noload $_src $MNT || die "cannot mount the installed system"
    _old=$(cat $MNT/etc/mu5358/version 2>/dev/null)
    _new=$(cat $IN/version 2>/dev/null)
    if [ -n "$_old" ] && [ -n "$_new" ] && [ "${MU5358_ALLOW_DOWNGRADE:-}" != 1 ] && [ "$(tag_cmp "$_old" "$_new")" = newer ]; then
        umount $MNT; die "installed $_old is newer than $_new (MU5358_ALLOW_DOWNGRADE=1 to allow)"
    fi
    (cd $MNT && tar -cf $HOME_DIR/carry.tar $(for p in $CARRY; do [ -e "$p" ] && echo "$p"; done) 2>/dev/null) || true
    say "carrying $(tar -tf $HOME_DIR/carry.tar 2>/dev/null | wc -l) files from $_old"
    umount $MNT
    [ "$TARGET" = region ] && /system/bin/losetup -d $_lo
}
carry_restore() {  # into the system mounted at $1
    [ -s $HOME_DIR/carry.tar ] || return 0
    tar -xf $HOME_DIR/carry.tar -C "$1" || { say "carry"; return 1; }
    # the new release's first-boot defaults for the settings carried must not run again over them
    rm -f "$1"/etc/uci-defaults/90-mu5358 "$1"/etc/uci-defaults/95-mu5358
    say "carried the settings"
}

system() {
    step system
    _lo=
    if [ "$TARGET" = fulldumpdb ]; then
        _d=$BYNAME/fulldumpdb _seek=0
    else
        _d=$DISK _seek=$((R0 / 2048))
    fi
    say "writing the system into $TARGET"
    gzip -dc $IN/$F_ROOT | dd of=$_d bs=1048576 seek=$_seek conv=fsync 2>/dev/null || die "$TARGET: write failed"
    sync; echo 3 > /proc/sys/vm/drop_caches
    if [ "$TARGET" = region ]; then
        # a loop device over the region (it is not a partition), the filesystem grown to the region less its log
        # Android's own losetup finds and sets up a free one in one step (busybox's -f, then a set-up, failed
        # inside the job once: "No such file or directory", 2026-10-30)
        _lo=$(/system/bin/losetup -f -s -o $((R0 * 512)) $DISK 2>&1) && [ -b "$_lo" ] ||
            die "no loop device: $_lo"
        _d=$_lo
    fi
    _want=$(sed -n "s/^# image $F_ROOT \([0-9a-f]\{64\}\) \([0-9]*\)$/\1 \2/p" $IN/$F_SUMS)
    if [ -n "$_want" ]; then
        set -- $_want
        [ "$(head -c "$2" $_d | sha256sum | cut -c1-64)" = "$1" ] || die "$TARGET: read back differs"
        say "read back: the image's checksum"
    fi
    [ "$(label $_d)" = mu5358root ] || die "$TARGET: no mu5358root label"
    if [ -n "$_lo" ]; then
        /system/bin/e2fsck -fy $_lo >/dev/null 2>&1
        /system/bin/resize2fs $_lo $((RSIZE / 4096)) >/dev/null 2>&1 || die "resize2fs to the region failed"
        say "the filesystem: $((RSIZE / 1048576)) MiB"
    fi
    step vendor
    mkdir -p $MNT
    umount $MNT 2>/dev/null
    mount -t ext4 -o rw,noatime $_d $MNT || die "cannot mount the system"
    [ -e $MNT/etc/openwrt_release ] || { umount $MNT; die "not an OpenWrt system"; }
    vendor_files $MNT || { umount $MNT; die "vendor files"; }
    [ "$UPDATE" = 1 ] && { carry_restore $MNT || { umount $MNT; die "carry"; }; }
    sync
    umount $MNT || die "cannot unmount the system"
    if [ -x /system/bin/e2fsck ]; then
        /system/bin/e2fsck -fn $_d >/dev/null 2>&1 || die "the system's filesystem does not check clean"
        say "e2fsck: clean"
    fi
    [ -n "$_lo" ] && /system/bin/losetup -d $_lo
    # only one system: boot/init takes fulldumpdb's before the region's
    if [ "$TARGET" = region ] && [ "$(fd_state)" = ours ]; then
        dd if=/dev/zero of=$BYNAME/fulldumpdb bs=1048576 conv=fsync 2>/dev/null; say "fulldumpdb: back to zeros"
    elif [ "$TARGET" = fulldumpdb ] && [ "$RSTATE" = ours ]; then
        dd if=/dev/zero of=$DISK bs=1048576 seek=$((R0 / 2048)) count=4 conv=fsync 2>/dev/null; say "the region: label erased"
    fi
}

boot_images() {
    step boot
    W=$HOME_DIR/work
    rm -rf $W && mkdir -p $W/rd/mu/modules $W/rd/dev || die "work directory"
    dd if=$BYNAME/boot_a of=$W/boot_a.img bs=1048576 2>/dev/null &&
        dd if=$BYNAME/init_boot_a of=$W/init_boot_a.img bs=1048576 2>/dev/null || die "cannot read slot a"
    gzip -dc $IN/$F_KERNEL > $W/Image || die "kernel"
    [ "$(dd if=$W/Image bs=1 skip=56 count=4 2>/dev/null | hex)" = 41524d64 ] || die "not an arm64 kernel"
    bootimg_put $W/boot_a.img $W/Image kernel $W/boot.img || die "boot image"
    # the ramdisk: the release's init and busybox, misc's Android block for init to put back, the extra module;
    # the module order init works out itself from vendor_boot's modules.dep
    tar -xzf $IN/$F_RAMDISK -C $W/rd || die "ramdisk"
    [ -f $W/rd/init ] && [ -f $W/rd/mu/busybox ] || die "ramdisk: no init or busybox"
    chmod 755 $W/rd/init $W/rd/mu/busybox
    [ -f $W/rd/mu/e2fsck ] && chmod 755 $W/rd/mu/e2fsck
    unhex "$(cat $HOME_DIR/bc-android.hex)" > $W/rd/mu/misc-bc-android.bin
    for _m in $RD_MODS; do cp /vendor_dlkm/lib/modules/$_m.ko $W/rd/mu/modules/ || die "no $_m.ko"; done
    mknod $W/rd/dev/console c 5 1 || die "mknod"
    (cd $W/rd && find init mu dev | cpio -o -H newc 2>/dev/null) > $W/rd.cpio || die "cpio"
    # LZ4 legacy, like vendor_boot's ramdisk in front of it (a gzip segment behind it is never unpacked)
    /data/adb/magisk/magiskboot compress=lz4_legacy $W/rd.cpio $W/rd.lz4 >/dev/null 2>&1 || die "lz4"
    [ "$(head -c 4 $W/rd.lz4 | hex)" = 02214c18 ] || die "lz4: not LZ4 legacy"
    /data/adb/magisk/magiskboot decompress $W/rd.lz4 $W/rd.chk >/dev/null 2>&1 && cmp -s $W/rd.chk $W/rd.cpio ||
        die "lz4: does not decompress to the ramdisk"
    bootimg_put $W/init_boot_a.img $W/rd.lz4 ramdisk $W/init_boot.img || die "init_boot image"
    say "boot.img $(fsize $W/boot.img), init_boot.img $(fsize $W/init_boot.img), ramdisk $(fsize $W/rd.lz4)"
    write_part $W/boot.img $BYNAME/boot_b || die "boot_b"
    write_part $W/init_boot.img $BYNAME/init_boot_b || die "init_boot_b"
    { echo "version=$(cat $IN/version 2>/dev/null)"; echo "date=$(date -u +%FT%TZ)"; echo "target=$TARGET"
      echo "boot_b=$(sha $W/boot.img)"; echo "boot_b_head=$(head -c 1048576 $W/boot.img | sha256sum | cut -c1-64)"
      echo "init_boot_b=$(sha $W/init_boot.img)"; } > $HOME_DIR/installed
    rm -rf $W
}

install() {
    TARGET=${1:-region} LINUX_MIB=${2:-} UPDATE=0
    case $TARGET in region|fulldumpdb) ;; *) die "install region [MIB] | fulldumpdb" ;; esac
    check; backup; space; chain; system; boot_images
    rm -f $IN/$F_ROOT $IN/$F_KERNEL $IN/$F_RAMDISK $IN/shadow-hash
    step done
    say "installed $(installed version); Linux starts with boot-linux, once"
}

update() {
    UPDATE=1 LINUX_MIB=
    TARGET=$(installed target)
    [ -n "$TARGET" ] || die "nothing installed to update"
    MU5358_NO_PASSWORD=1 check
    region_info
    case $TARGET in
        region) [ "$RSTATE" = ours ] || die "the region has no system" ;;
        fulldumpdb) [ "$(fd_state)" = ours ] || die "fulldumpdb has no system" ;;
    esac
    carry_save; chain; system; boot_images
    rm -f $IN/$F_ROOT $IN/$F_KERNEL $IN/$F_RAMDISK $HOME_DIR/carry.tar
    step done
    say "updated to $(installed version); Linux starts with boot-linux, once"
}

# ---- uninstall -------------------------------------------------------------------------------------------------
# Android first (misc), then slot b's boot and init_boot back to Android's (slot a's), then the system erased:
# whatever stops it, the device starts Android.
uninstall() {
    step misc
    _bc=$(bc_live)
    case $(bc_state "$_bc") in
        android) ;;
        trial) [ -f $HOME_DIR/bc-android.hex ] && bc_write "$(cat $HOME_DIR/bc-android.hex)" || die "misc" ;;
        *) die "misc is neither Android's nor a trial: left alone" ;;
    esac
    step boot
    for _c in boot init_boot; do
        dd if=$BYNAME/${_c}_a of=$BYNAME/${_c}_b bs=1048576 conv=fsync 2>/dev/null; sync
        echo 3 > /proc/sys/vm/drop_caches
        cmp -s $BYNAME/${_c}_a $BYNAME/${_c}_b || die "${_c}_b: read back differs"
    done
    step erase
    region_info || die "cannot read the GPT"
    if [ "$RSTATE" = ours ]; then
        dd if=/dev/zero of=$DISK bs=1048576 seek=$((R0 / 2048)) count=4 conv=fsync 2>/dev/null
        say "the region: label erased"
    fi
    if [ "${1:-}" != keep-space ] && [ $RSIZE -gt 0 ]; then
        # vold grows userdata's f2fs to the partition at Android's next start
        $HOME_DIR/gpt-userdata $DISK $LAST || die "the GPT write failed"
        say "the region goes back to userdata at the next start of Android"
    fi
    if [ "$(fd_state)" = ours ]; then
        # back to zeros, as the firmware left it
        blkdiscard -z $BYNAME/fulldumpdb 2>/dev/null ||
            dd if=/dev/zero of=$BYNAME/fulldumpdb bs=1048576 conv=fsync 2>/dev/null
        sync; echo 3 > /proc/sys/vm/drop_caches
        zero $BYNAME/fulldumpdb || die "fulldumpdb is not zeros"
    fi
    rm -f $HOME_DIR/installed
    step done
}

# ---- boot-linux ------------------------------------------------------------------------------------------------
boot_linux() {
    [ -f $HOME_DIR/installed ] || die "nothing installed"
    if [ "$(installed target)" = fulldumpdb ]; then
        [ "$(fd_state)" = ours ] || die "fulldumpdb has no system"
    else
        region_info && [ "$RSTATE" = ours ] || die "the region has no system"
    fi
    [ "$(sha256sum < $BYNAME/boot_b | cut -c1-64)" = "$(installed boot_b)" ] || die "boot_b is not the installed one"
    [ "$(sha256sum < $BYNAME/init_boot_b | cut -c1-64)" = "$(installed init_boot_b)" ] || die "init_boot_b is not the installed one"
    _bc=$(bc_live)
    [ "$(bc_state "$_bc")" = android ] || die "misc is not Android's ($(bc_state "$_bc"))"
    # the ramdisk puts back the block it was built with: it must still be the live one
    [ "$_bc" = "$(cat $HOME_DIR/bc-android.hex)" ] || die "misc changed since the installation: install again"
    _t=$(bc_trial "$_bc")
    bc_valid "$_t" || die "trial block"
    [ "${1:-}" = dry ] && { echo "MU5358-INFO trial $_t"; return 0; }
    bc_write "$_t" || { bc_write "$_bc"; die "misc: read back differs (put back)"; }
    echo "MU5358-REBOOT"
    sync
    (sleep 1; reboot) >/dev/null 2>&1 &
}

# ---- jobs ------------------------------------------------------------------------------------------------------
run() {
    case ${1:-} in install|update|uninstall) ;; *) die "run install|update|uninstall" ;; esac
    _args=$(printf '%s' "${2:-} ${3:-}" | tr -cd 'a-z0-9 -')
    mkdir -p $JOB && chmod 700 $HOME_DIR
    if [ -f $JOB/pid ] && kill -0 "$(cat $JOB/pid)" 2>/dev/null; then die "a job is running"; fi
    rm -f $JOB/pid $JOB/exit $JOB/log
    cp "$0" $HOME_DIR/mu5358-install.sh && cp "$HERE/android-files.txt" $HOME_DIR/android-files.txt ||
        die "cannot copy the installer"
    setsid sh -c "echo \$\$ > $JOB/pid; MU5358_IN_BB=1 ASH_STANDALONE=1 $BB sh $HOME_DIR/mu5358-install.sh _$1 $_args > $JOB/log 2>&1; echo \$? > $JOB/exit" \
        < /dev/null > /dev/null 2>&1 &
    _i=0; while [ ! -f $JOB/pid ] && [ $_i -lt 50 ]; do sleep 0.1; _i=$((_i + 1)); done
    echo "MU5358-JOB started"
}
follow() {
    [ -f $JOB/log ] || die "no job"
    tail -n +1 -f $JOB/log &
    _t=$!
    while [ ! -f $JOB/exit ]; do sleep 1; done
    sleep 1; kill $_t 2>/dev/null
    echo "MU5358-EXIT $(cat $JOB/exit)"
}

case ${1:-} in
    status) status ;;
    take) take "${2:-}" ;;
    password) password ;;
    run) run "${2:-}" "${3:-}" "${4:-}" ;;
    _install) install "${2:-}" "${3:-}" ;;
    _update) update ;;
    _uninstall) uninstall "${2:-}" ;;
    follow) follow ;;
    boot-linux) boot_linux "${2:-}" ;;
    backup) [ -f $BACKUP/SHA256SUMS ] || die "no backup"; (cd $BACKUP && for f in *.gz *.bin SHA256SUMS; do [ -f "$f" ] || continue; echo "MU5358-FILE $f $(fsize $f)"; done) ;;
    cat) case ${2:-} in boot_b.img.gz|init_boot_b.img.gz|misc-4k.bin|gpt-primary.bin|gpt-backup.bin|SHA256SUMS) base64 $BACKUP/$2 ;; *) die "no such file" ;; esac ;;
    _test) shift; "$@" ;;
    *) sed -n '2,/^set -u/p' "$0" | sed '$d'; exit 2 ;;
esac
