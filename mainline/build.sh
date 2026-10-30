#!/bin/bash
# Build mainline for the U30 Pro inside mu5358-mainline-build (mainline/Dockerfile, mu300-linux's): /work = this
# directory, /src = a volume for the kernel tree. Adapted from mu300-linux upstream/build.sh, without its UMS9620
# port: allnoconfig + mu5358-mainline.config + patches/, Image into /work/out.
#   docker run --rm -v mu5358-mainline:/src -v "$PWD/mainline":/work mu5358-mainline-build bash /work/build.sh
set -eo pipefail
KV=${KV:-6.18.59}
OUT=/work/${OUTDIR:-out}
# the source, fetched once and unpacked only after both signatures check out (verify-kernel-src.sh, from mu300)
if [ ! -d /src/linux-$KV ]; then
    base=https://cdn.kernel.org/pub/linux/kernel/v${KV%%.*}.x
    dl=/src/.download-$KV
    rm -rf "$dl" && mkdir -p "$dl"
    for f in linux-$KV.tar.xz linux-$KV.tar.sign sha256sums.asc; do
        u=$base/$f; [ "$f" != sha256sums.asc ] || u=https://www.kernel.org/pub/linux/kernel/v${KV%%.*}.x/$f
        curl -fsSL -o "$dl/$f" "$u" || { rm -rf "$dl"; echo "cannot fetch $u" >&2; exit 1; }
    done
    bash /work/verify-kernel-src.sh "$dl" "$KV" || { rm -rf "$dl"; exit 1; }
    tar -xJf "$dl/linux-$KV.tar.xz" -C "$dl" && mv "$dl/linux-$KV" /src/linux-$KV
    rm -rf "$dl"
fi
cd /src/linux-$KV
# the patches, in order. A copy of each applied one is kept in .mu5358-applied/: when patches/ changes, the tree
# goes back (newest first) to the last one that is still the same, and the rest are applied again; a patch that does
# not apply stops the build
A=.mu5358-applied
mkdir -p $A
want=$(cd /work/patches && ls *.patch 2>/dev/null)
keep=
for n in $want; do
    [ -e $A/$n ] && cmp -s $A/$n /work/patches/$n || break
    keep="$keep $n"
done
for n in $(ls -r $A); do
    case " $keep " in *" $n "*) continue;; esac
    patch -p1 -R -s < $A/$n || { echo "cannot take back $n" >&2; exit 1; }
    rm $A/$n
done
for n in $want; do
    [ -e $A/$n ] && continue
    patch -p1 -N -s --dry-run < /work/patches/$n >/dev/null 2>&1 || { echo "patch does not apply to $KV: $n" >&2; exit 1; }
    patch -p1 -N -s < /work/patches/$n
    cp /work/patches/$n $A/$n
done
O=/src/out-$KV
mkdir -p $O
make O=$O ARCH=arm64 allnoconfig >/dev/null
./scripts/kconfig/merge_config.sh -m -O $O $O/.config /work/mu5358-mainline.config >/dev/null
make O=$O ARCH=arm64 olddefconfig >/dev/null
# options Kconfig did not take stop the build
bad=
while IFS= read -r l; do
  case "$l" in CONFIG_*=*) k=${l%%=*}; v=${l#*=}; g=$(grep -E "^$k=" $O/.config | cut -d= -f2- || true)
    [ "$v" = n ] && { grep -q "^$k=y" $O/.config && bad="$bad\nNOT DISABLED: $k" || true; continue; }
    [ "$g" = "$v" ] || bad="$bad\nNOT SET: $k want $v got ${g:-unset}";; esac
done < /work/mu5358-mainline.config
[ -z "$bad" ] || { printf "config options not taken:$bad\n" >&2; exit 1; }
make O=$O ARCH=arm64 -j"$(nproc)" Image > $O/build.log 2>&1 || {
    grep -n -E ": (fatal )?error: |undefined reference|No such file|Killed|\*\*\*" -A3 $O/build.log | head -60 || true
    tail -20 $O/build.log; exit 1
}
mkdir -p $OUT
cp $O/arch/arm64/boot/Image $O/.config $OUT/
ls -la $OUT
