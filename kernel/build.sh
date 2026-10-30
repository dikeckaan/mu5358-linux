#!/bin/bash
# Build the U30 Pro's GKI kernel (android15-6.6 at 9a8b79fdfb2e, the device's own commit) with the stock config plus
# mu5358-linux.fragment. Runs in the container of kernel/Dockerfile: /src = the case-sensitive volume holding
# common/, /work = this directory.
set -e
cd /src/common
# kernel/patches/*.patch, each once: one that applies in reverse is in already
for p in /work/patches/*.patch; do
    if patch -p1 -R --dry-run -s -f < "$p" >/dev/null 2>&1; then echo "patch in: ${p##*/}"
    else patch -p1 -N -s < "$p" && echo "patch applied: ${p##*/}"; fi
done
OUT=/src/out
mkdir -p $OUT
M="make O=$OUT ARCH=arm64 LLVM=1 LLVM_IAS=1 -j${JOBS:-10}"
cp /work/stock-gki-6.6.98.config $OUT/.config
KCONFIG_CONFIG=$OUT/.config ./scripts/kconfig/merge_config.sh -m -O $OUT $OUT/.config /work/mu5358-linux.fragment > /work/merge.log 2>&1
$M olddefconfig
python3 - <<'P'
import re
def parse(p):
    d = {}
    for l in open(p):
        m = re.match(r'(CONFIG_[A-Z0-9_]+)=(.*)', l) or re.match(r'# (CONFIG_[A-Z0-9_]+) is not set', l)
        if m: d[m.group(1)] = m.group(2) if m.lastindex == 2 else 'n'
    return d
want, got = parse('/work/mu5358-linux.fragment'), parse('/src/out/.config')
bad = [f'{k}: want {v} got {got.get(k, "absent")}' for k, v in want.items() if got.get(k, 'n') != v]
stock = parse('/work/stock-gki-6.6.98.config')
changed = [f'{k}: {stock.get(k, "absent")} -> {got.get(k, "absent")}' for k in sorted(set(stock) | set(got))
           if stock.get(k, 'n') != got.get(k, 'n') and k not in want]
open('/work/fragment-rejected.txt', 'w').write('\n'.join(bad) + '\n')
open('/work/config-side-effects.txt', 'w').write('\n'.join(changed) + '\n')
print('fragment rejected:', len(bad), ' other options changed:', len(changed))
P
cp $OUT/.config /work/linux.config
start=$(date +%s)
$M Image modules > /work/build.log 2>&1 || status=$?
grep -E "error:|Error " /work/build.log | tail -20 || true
[ -z "${status:-}" ] || { echo "kernel build failed (make exit $status)"; exit "$status"; }
echo "BUILD_SECONDS $(( $(date +%s) - start ))"
ls -la $OUT/arch/arm64/boot/Image
cp $OUT/Module.symvers /work/Module.symvers
strings $OUT/arch/arm64/boot/Image | grep -m1 'Linux version'
