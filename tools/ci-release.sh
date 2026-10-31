#!/bin/bash
# The whole release, from the sources alone, on a GitHub arm64 runner (.github/workflows/release.yml) or on any
# arm64 Linux with docker and gh:
#   tools/ci-release.sh TAG [--publish] [--prerelease]
# What it does, in order, each piece the same script a maintainer runs by hand:
#   1. the GKI kernel: android15-6.6 at the device's own commit (ACK_COMMIT), shallow, into work/kernel/common;
#      kernel/build.sh in kernel/Dockerfile's image; the result must be the kernel the vendor modules load into
#      (kernel.release, and every exported symbol's CRC as kernel/Module.symvers records them)
#   2. the tools: u30ui, u30ui2, screencon (mainline/Dockerfile's image), panelcmd.ko against that kernel,
#      e2fsck (tools/e2fsck/build.sh), the ramdisk's busybox (Ubuntu 24.04's busybox-static, as mu300-linux's
#      releases carry it), gltest (gpu/prebuilt: it links against the device's own Mali library)
#   3. the base: mu300-linux's openwrt-luci rootfs, by the digest tools/make-release.sh pins
#   4. tools/make-release.sh TAG: the image, kernel, ramdisk, tools and the signed SHA256SUMS (the key as text in
#      MU5358_RELEASE_KEY, the repository secret; without it the build stops: a release must be signed)
#   5. --publish: the GitHub release (--prerelease: as a test release, which the web installer lists but does not
#      propose), then tools/publish-mirror.sh (the branch mirror/TAG the page downloads from)
# Everything lands in work/ (never in git). GH_TOKEN (the workflow's) for gh and the pushes.
set -eu
cd "$(dirname "$0")/.."
TAG=${1:?usage: tools/ci-release.sh TAG [--publish] [--prerelease]}
shift
PUBLISH= PRERELEASE=
for a in "$@"; do case $a in --publish) PUBLISH=1 ;; --prerelease) PRERELEASE=1 ;; *) echo "unknown: $a" >&2; exit 2 ;; esac; done
case $TAG in v[0-9][0-9][0-9][0-9].[0-9][0-9].[0-9][0-9].[1-9]|v[0-9][0-9][0-9][0-9].[0-9][0-9].[0-9][0-9].[1-9][0-9]) ;; *) echo "not a release tag: $TAG" >&2; exit 2 ;; esac
[ "$(uname -m)" = aarch64 ] || { echo "this builds on arm64 only (the kernel image is native, like kernel/Dockerfile)" >&2; exit 1; }

ACK_REPO=https://android.googlesource.com/kernel/common
ACK_COMMIT=9a8b79fdfb2e7f1c136272e341bc29fc448a1b3a        # kernel/build.sh: "android15-6.6 at 9a8b79fdfb2e"
KREL_WANT=6.6.98-android15-8-mu5358-4k                        # the kernel the vendor modules were loaded into (docs/GKI-SYSTEM.md)
ROOTFS_TAG=v2026.10.29.1                                      # tools/make-release.sh pins its digest
ROOTFS_NAME=mu300-openwrt-luci-rootfs.tar.gz
W=$PWD/work
T0=$(date +%s)
step() { echo; echo "==> $* ($(( $(date +%s) - T0 )) s)"; }

step "the build images"
docker build -q -t mu5358-kbuild kernel >/dev/null
docker build -q -t mu5358-mainline-build mainline >/dev/null

step "the kernel source: $ACK_REPO at $ACK_COMMIT"
mkdir -p "$W/kernel"
if [ ! -f "$W/kernel/common/Makefile" ]; then
    rm -rf "$W/kernel/common"
    git init -q "$W/kernel/common"
    git -C "$W/kernel/common" fetch -q --depth 1 "$ACK_REPO" "$ACK_COMMIT"
    git -C "$W/kernel/common" checkout -q FETCH_HEAD
    [ "$(git -C "$W/kernel/common" rev-parse HEAD)" = "$ACK_COMMIT" ] || { echo "the fetch did not give $ACK_COMMIT" >&2; exit 1; }
    # a plain tree from here on, as the maintainer's (a tarball): setlocalversion must not see a git repository
    # and add "+" or a hash to the kernel's release string
    echo "$ACK_COMMIT" > "$W/kernel/common/.ack-commit"
    rm -rf "$W/kernel/common/.git"
fi
[ "$(cat "$W/kernel/common/.ack-commit")" = "$ACK_COMMIT" ] || { echo "work/kernel/common is not at $ACK_COMMIT" >&2; exit 1; }
grep -q '^SUBLEVEL = 98$' "$W/kernel/common/Makefile" || { echo "not a 6.6.98 tree" >&2; exit 1; }

step "the kernel (kernel/build.sh)"
# the KMI record as committed: kernel/build.sh writes the build's Module.symvers over kernel/Module.symvers (the record,
# on the maintainer's Mac), so the comparison below takes the committed file before the build runs
mkdir -p "$W/ci"
git show "${GITHUB_SHA:-HEAD}":kernel/Module.symvers > "$W/ci/Module.symvers.ref"
[ "$(wc -l < "$W/ci/Module.symvers.ref")" -gt 10000 ] || { echo "kernel/Module.symvers of record is not there" >&2; exit 1; }
docker run --rm -e JOBS="$(nproc)" -v "$W/kernel":/src -v "$PWD/kernel":/work mu5358-kbuild bash /work/build.sh
KOUT=$W/kernel/out
KREL=$(cat "$KOUT/include/config/kernel.release")
[ "$KREL" = "$KREL_WANT" ] || { echo "kernel.release is $KREL, not $KREL_WANT" >&2; exit 1; }
# the KMI: every exported symbol with the CRC the modules of the device were loaded against (kernel/Module.symvers,
# the maintainer's build that the stock vendor modules were verified to load into, kernel/check-kmi.py)
python3 - "$KOUT/Module.symvers" "$W/ci/Module.symvers.ref" <<'P'
import sys
def read(p):
    out = {}
    for l in open(p):
        f = l.split()
        if len(f) >= 2: out[f[1]] = f[0].lower()
    return out
new, ref = read(sys.argv[1]), read(sys.argv[2])
missing = sorted(s for s in ref if s not in new)
differ = sorted(s for s in ref if s in new and new[s] != ref[s])
print(f"KMI: {len(ref)} symbols of record, {len(new)} built; missing {len(missing)}, CRC differs {len(differ)}")
for s in (missing + differ)[:40]: print("  ", s, ref[s], new.get(s, "-"))
sys.exit(1 if missing or differ else 0)
P
# every option of the fragment must have survived olddefconfig (kernel/build.sh writes what did not; the file holds a
# newline when all did): a module the system counts on must not go missing quietly
grep -q . kernel/fragment-rejected.txt && { echo "the fragment was not fully applied:" >&2; cat kernel/fragment-rejected.txt >&2; exit 1; }
# the kernel's build log and config, for the record (uploaded by the workflow)
cp kernel/build.log kernel/linux.config kernel/config-side-effects.txt kernel/fragment-rejected.txt kernel/Module.symvers "$W/ci/"

step "the tools"
docker run --rm -v "$PWD/tools/u30ui":/w mu5358-mainline-build bash /w/build.sh
docker run --rm -v "$PWD/tools":/t mu5358-mainline-build bash /t/u30ui2/build.sh
docker run --rm -v "$PWD/tools":/t mu5358-mainline-build bash /t/charge/build.sh
docker run --rm -e FONT=/src/common/lib/fonts/font_6x10.c -v "$W/kernel":/src -v "$PWD/tools/screencon":/w mu5358-mainline-build bash /w/build.sh
docker run --rm -v "$W/kernel":/src -v "$PWD/tools/spispeed":/m mu5358-kbuild bash /m/build.sh
VERMAGIC=$(docker run --rm -v "$PWD/tools/spispeed":/m mu5358-kbuild modinfo -F vermagic /m/panelcmd.ko)
case $VERMAGIC in "$KREL_WANT "*) ;; *) echo "panelcmd.ko is for '$VERMAGIC', not $KREL_WANT" >&2; exit 1 ;; esac
[ -f "$W/e2fsck/e2fsck" ] || sh tools/e2fsck/build.sh "$W/e2fsck/e2fsck"
cp gpu/prebuilt/gltest gpu/gltest && chmod 755 gpu/gltest
# the ramdisk's busybox: the static one of Ubuntu 24.04, from the image kernel/Dockerfile is built on
mkdir -p "$W/in"
docker run --rm mu5358-kbuild sh -c 'apt-get update -qq >/dev/null 2>&1 && apt-get install -y -qq busybox-static >/dev/null 2>&1 && dpkg -s busybox-static | grep ^Version >&2 && cat /bin/busybox' > "$W/in/busybox"
chmod 755 "$W/in/busybox"
case $(file "$W/in/busybox") in *"statically linked"*) ;; *) echo "busybox is not static" >&2; exit 1 ;; esac

step "the base: mu300-linux $ROOTFS_TAG $ROOTFS_NAME"
[ -f "$W/in/$ROOTFS_NAME" ] || gh release download "$ROOTFS_TAG" --repo dikeckaan/mu300-linux --pattern "$ROOTFS_NAME" --dir "$W/in"

step "tools/make-release.sh $TAG"
MU5358_REQUIRE_SIGNATURE=1 KOUT=$KOUT BUSYBOX=$W/in/busybox NDKCC=/nonexistent sh tools/make-release.sh "$TAG" "$W/in/$ROOTFS_NAME"
OUT=$W/release/$TAG
python3 -I tools/usign-verify.py installer/release.pub "$OUT/SHA256SUMS.sig" "$OUT/SHA256SUMS"
(cd "$OUT" && grep -v '^#' SHA256SUMS | sha256sum -c - --quiet)
ls -la "$OUT"

[ -n "$PUBLISH" ] || { step "built, not published (no --publish)"; exit 0; }
step "the release $TAG on GitHub${PRERELEASE:+ (prerelease)}"
REPO=${GITHUB_REPOSITORY:-dikeckaan/mu5358-linux}
SHA=${GITHUB_SHA:-$(git rev-parse HEAD)}
gh api "repos/$REPO/releases/tags/$TAG" >/dev/null 2>&1 && { echo "$TAG is a release already" >&2; exit 1; }
{
    echo "MU5358 Linux for the ZTE U30 Pro, installed from the browser: https://kaandikec.com/mu5358-linux/web-installer/"
    echo
    echo "Built by the Release workflow from \`${SHA}\` (kernel \`$KREL\`, base mu300-linux $ROOTFS_TAG). Linux boots only as a one-time trial from slot b; Android stays the default and nothing of slot a or the modem's NV partitions is written. The installer puts the system behind userdata (an online, wipe-free f2fs shrink) or into fulldumpdb."
    echo
    echo "\`SHA256SUMS\` is signed with the project's release key (\`installer/release.pub\`, \`SHA256SUMS.sig\`, usign/signify Ed25519); the web installer and the device refuse a release without a valid signature."
    [ -z "$PRERELEASE" ] || { echo; echo "A test release: the web installer lists it but proposes the newest release that is not one."; }
} > "$W/ci/notes.md"
gh release create "$TAG" --repo "$REPO" --target "$SHA" --title "$TAG" ${PRERELEASE:+--prerelease} --notes-file "$W/ci/notes.md" \
    "$OUT/SHA256SUMS" "$OUT/SHA256SUMS.sig" "$OUT/android-files.txt" "$OUT/mu5358-install.sh" "$OUT/mu5358-kernel.Image.gz" \
    "$OUT/mu5358-openwrt.img.gz" "$OUT/mu5358-ramdisk.tar.gz" "$OUT/mu5358-tools.tar.gz"
# the assets as GitHub holds them: the digests GitHub computed (what the page pins) must be the files' (GitHub
# computes them on upload; a few tries in case one is not there yet)
for try in 1 2 3 4 5 6; do
    gh api "repos/$REPO/releases/tags/$TAG" --jq '.assets[] | "\(.digest // "none")  \(.name)"' | sed 's/^sha256://' > "$W/ci/assets.sums"
    [ "$(wc -l < "$W/ci/assets.sums")" = 8 ] && ! grep -q '^none' "$W/ci/assets.sums" && break
    sleep 10
done
(cd "$OUT" && sha256sum -c - --quiet < "$W/ci/assets.sums") || { echo "the assets on GitHub are not the files built" >&2; cat "$W/ci/assets.sums"; exit 1; }
step "the mirror (tools/publish-mirror.sh)"
MU5358_REPO=$REPO sh tools/publish-mirror.sh "$TAG"
step "done: https://github.com/$REPO/releases/tag/$TAG"
