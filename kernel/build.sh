#!/bin/bash
# Build the U30 Pro's GKI kernel (android15-6.6 at 9a8b79fdfb2e, the device's own commit) with the stock config plus
# mu5358-linux.fragment. Runs in the container of kernel/Dockerfile: SRC (/src) = the case-sensitive volume holding
# common/, WORK (/work) = this directory; JOBS = the parallelism (all processors). The Release workflow
# (.github/workflows/release.yml, tools/ci-release.sh) runs it the same way on a GitHub arm64 runner.
set -e
SRC=${SRC:-/src}; WORK=${WORK:-/work}
cd $SRC/common
# kernel/patches/*.patch, each once: one that applies in reverse is in already
for p in $WORK/patches/*.patch; do
    if patch -p1 -R --dry-run -s -f < "$p" >/dev/null 2>&1; then echo "patch in: ${p##*/}"
    else patch -p1 -N -s < "$p" && echo "patch applied: ${p##*/}"; fi
done
OUT=$SRC/out
mkdir -p $OUT
M="make O=$OUT ARCH=arm64 LLVM=1 LLVM_IAS=1 -j${JOBS:-$(nproc)}"
cp $WORK/stock-gki-6.6.98.config $OUT/.config
KCONFIG_CONFIG=$OUT/.config ./scripts/kconfig/merge_config.sh -m -O $OUT $OUT/.config $WORK/mu5358-linux.fragment > $WORK/merge.log 2>&1
$M olddefconfig
WORK=$WORK SRC=$SRC python3 - <<'P'
import os, re
WORK, SRC = os.environ['WORK'], os.environ['SRC']
def parse(p):
    d = {}
    for l in open(p):
        m = re.match(r'(CONFIG_[A-Z0-9_]+)=(.*)', l) or re.match(r'# (CONFIG_[A-Z0-9_]+) is not set', l)
        if m: d[m.group(1)] = m.group(2) if m.lastindex == 2 else 'n'
    return d
want, got = parse(WORK + '/mu5358-linux.fragment'), parse(SRC + '/out/.config')
bad = [f'{k}: want {v} got {got.get(k, "absent")}' for k, v in want.items() if got.get(k, 'n') != v]
stock = parse(WORK + '/stock-gki-6.6.98.config')
changed = [f'{k}: {stock.get(k, "absent")} -> {got.get(k, "absent")}' for k in sorted(set(stock) | set(got))
           if stock.get(k, 'n') != got.get(k, 'n') and k not in want]
open(WORK + '/fragment-rejected.txt', 'w').write('\n'.join(bad) + '\n')
open(WORK + '/config-side-effects.txt', 'w').write('\n'.join(changed) + '\n')
print('fragment rejected:', len(bad), ' other options changed:', len(changed))
P
cp $OUT/.config $WORK/linux.config
start=$(date +%s)
$M Image modules > $WORK/build.log 2>&1 || status=$?
grep -E "error:|Error " $WORK/build.log | tail -20 || true
[ -z "${status:-}" ] || { echo "kernel build failed (make exit $status)"; exit "$status"; }
echo "BUILD_SECONDS $(( $(date +%s) - start ))"
ls -la $OUT/arch/arm64/boot/Image
cp $OUT/Module.symvers $WORK/Module.symvers
strings $OUT/arch/arm64/boot/Image | grep -m1 'Linux version'
