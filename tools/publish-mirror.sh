#!/bin/sh
# Publish a release's mirror for the web installer: the branch mirror/TAG of this repository, holding the release's
# files in parts and a manifest (tools/make-release-mirror.sh), which raw.githubusercontent.com serves to the page
# (site/assets/web-installer/config.js names the address; mirror.js reads it).
#   tools/publish-mirror.sh TAG [REMOTE]
# from work/release/TAG, after the release itself is on GitHub (the page only offers a mirror of a release it sees).
# A branch of its own, with no history (one commit, force-pushed): the repository's main history never carries
# the files. The branches of the newest release and the newest prerelease are kept, every other mirror/* branch
# is deleted, so that the repository stays small (a release is about 73 MB; GitHub prunes what no branch holds).
set -eu
cd "$(dirname "$0")/.."
TAG=${1:?usage: tools/publish-mirror.sh TAG [REMOTE]}
REMOTE=${2:-origin}
REPO=${MU5358_REPO:-dikeckaan/mu5358-linux}
SRC=work/release/$TAG
[ -f "$SRC/SHA256SUMS" ] || { echo "no $SRC/SHA256SUMS: build the release first (tools/make-release.sh $TAG)" >&2; exit 1; }
gh api "repos/$REPO/releases/tags/$TAG" --jq .tag_name >/dev/null 2>&1 || { echo "$TAG is not a release on GitHub yet: nothing mirrored" >&2; exit 1; }
OUT=$(mktemp -d "${TMPDIR:-/tmp}/mu5358-mirror.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
sh tools/make-release-mirror.sh "$SRC" "$OUT" "$TAG"
URL=$(git remote get-url "$REMOTE")
( cd "$OUT" && git init -q -b "mirror/$TAG" && git add -A &&
  git -c user.name='mu5358-linux release' -c user.email='noreply@github.com' commit -q -m "Mirror of $REPO $TAG for the web installer" &&
  git push -q --force "$URL" "HEAD:refs/heads/mirror/$TAG" )
echo "pushed: https://raw.githubusercontent.com/$REPO/refs/heads/mirror/$TAG/manifest.json"
keep=$(gh api "repos/$REPO/releases?per_page=30" --jq '[.[] | select(.draft | not) | select(.tag_name | startswith("v"))] | [(map(select(.prerelease | not)) | .[0].tag_name), (map(select(.prerelease)) | .[0].tag_name)] | .[] | select(. != null)')
for b in $(git ls-remote --heads "$URL" 'refs/heads/mirror/*' | awk '{ sub("^refs/heads/mirror/", "", $2); print $2 }'); do
    case $b in v[0-9]*) ;; *) continue ;; esac
    case $b in *[!0-9A-Za-z._]*) continue ;; esac
    [ "$b" != "$TAG" ] || continue
    echo "$keep" | grep -qxF "$b" && continue
    git push -q "$URL" --delete "refs/heads/mirror/$b" && echo "removed the mirror of $b" || echo "could not remove the mirror of $b"
done
