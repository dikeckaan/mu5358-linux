#!/bin/sh
# The next release tag of today: vYYYY.MM.DD.N (UTC), N one more than the highest N among the releases of today
# on GitHub (drafts included: a tag that exists must not be chosen again), 1 when there is none.
#   tools/next-tag.sh [REPO]
# With a tag as MU5358_TAG in the environment, that tag is checked instead: of today, in that form, and not yet
# a release or a tag; it is printed when it passes. (mu300-linux's --next-tag, as a script.)
set -eu
REPO=${1:-${MU5358_REPO:-dikeckaan/mu5358-linux}}
today=$(date -u +%Y.%m.%d)
have=$(gh api "repos/$REPO/releases?per_page=100" --jq '.[].tag_name'; gh api "repos/$REPO/git/matching-refs/tags/v" --jq '.[].ref' | sed 's|^refs/tags/||')
if [ -n "${MU5358_TAG:-}" ]; then
    t=$MU5358_TAG
    case $t in v"$today".[1-9]|v"$today".[1-9][0-9]) ;; *) echo "$t is not a tag of today in the form v$today.N" >&2; exit 1 ;; esac
    echo "$have" | grep -qxF "$t" && { echo "$t exists already" >&2; exit 1; }
    echo "$t"; exit 0
fi
n=$(echo "$have" | sed -n "s/^v$today\.\([1-9][0-9]\?\)$/\1/p" | sort -n | tail -n1)
echo "v$today.$(( ${n:-0} + 1 ))"
