#!/bin/sh
# Announce a release on the project's Telegram channel (.github/workflows/release.yml after a CI release, and
# .github/workflows/telegram.yml when a release is promoted by hand or on request). After mu300-linux's.
#   tools/telegram-announce.sh release TAG [published|released]
# Needs TELEGRAM_BOT_TOKEN and TELEGRAM_CHAT_ID (repository secrets), gh and jq. The message is short: what the
# release is, the first paragraph of its notes, and where to read on. Nothing from the environment but the two
# secrets goes out, and the token never reaches a command line or a log: curl takes the URL from a file.
# MU5358_TG_DRY=1 prints the message instead of sending it (a look at the text; tests/test_static.py).
set -eu
MODE=${1:-}; TAG=${2:-}; ACTION=${3:-published}
REPO=${GITHUB_REPOSITORY:-dikeckaan/mu5358-linux}
[ "$MODE" = release ] && [ -n "$TAG" ] || { echo "usage: $0 release TAG [published|released]" >&2; exit 2; }
case "$TAG" in v[0-9]*) ;; *) echo "not a release tag: $TAG" >&2; exit 2 ;; esac
case "$TAG" in *[!0-9A-Za-z._-]*|*..*) echo "not a release tag: $TAG" >&2; exit 2 ;; esac
: "${TELEGRAM_BOT_TOKEN:?}" "${TELEGRAM_CHAT_ID:?}"

esc() { sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g'; }
URL=https://github.com/$REPO/releases/tag/$TAG
PAGE=https://kaandikec.com/mu5358-linux/web-installer/
rel=$(gh release view "$TAG" -R "$REPO" --json name,isPrerelease,publishedAt,assets)
pre=$(printf '%s' "$rel" | jq -r .isPrerelease)
nassets=$(printf '%s' "$rel" | jq -r '.assets|length')
tag_esc=$(printf '%s' "$TAG" | esc)

# "released" also fires for a release published as a release straight away, right after "published": one message
# then, not two. A promotion comes later: the release has been there for a while.
if [ "$ACTION" = released ]; then
    age=$(( $(date +%s) - $(date -d "$(printf '%s' "$rel" | jq -r .publishedAt)" +%s 2>/dev/null || echo 0) ))
    [ "$age" -gt 600 ] || { echo "published just now: the 'published' event announces it"; exit 0; }
    kind="Now the latest release"
    lead="Tested on the board and promoted; the web installer proposes it from now on."
elif [ "$pre" = true ]; then
    kind="Pre-release"
    lead="Under test on the board; it becomes the latest release once it passes. The web installer lists it as a test release."
else
    kind="New release"
    lead="Install or update from the web installer: $PAGE"
fi
# the first paragraph of the release's notes (the lines up to the first blank one), at most 400 characters
notes=$(gh release view "$TAG" -R "$REPO" --json body --jq .body 2>/dev/null |
    awk 'NF == 0 { if (seen) exit; next } { seen = 1; print }' | tr '\n' ' ' | sed 's/`//g; s/ *$//' |
    awk '{ if (length($0) > 400) { s = substr($0, 1, 400); sub(/ [^ ]*$/, "", s); print s "…" } else print }' | esc) || notes=

text="<b>MU5358 Linux $tag_esc</b>
$kind

$lead"
[ -z "$notes" ] || text="$text

$notes"
text="$text

<a href=\"$URL\">Release page</a> ($nassets files) · <a href=\"$PAGE\">Web installer</a> · <a href=\"https://kaandikec.com/mu5358-linux/\">Site</a>"

[ "${MU5358_TG_DRY:-}" != 1 ] || { printf '%s\n' "$text"; exit 0; }
# the token stays out of the command line and the log: curl reads the URL from a config file
cfg=$(umask 077; mktemp "${TMPDIR:-/tmp}/mu5358-tg.XXXXXX"); trap 'rm -f "$cfg"' EXIT
printf 'url = "https://api.telegram.org/bot%s/sendMessage"\n' "$TELEGRAM_BOT_TOKEN" > "$cfg"
out=$(jq -n --arg c "$TELEGRAM_CHAT_ID" --arg t "$text" \
        '{chat_id: $c, text: $t, parse_mode: "HTML", disable_web_page_preview: true}' |
      curl -sS -K "$cfg" -H 'Content-Type: application/json' --data-binary @-)
printf '%s' "$out" | jq -e .ok >/dev/null || { echo "telegram: $(printf '%s' "$out" | jq -r '.description // "no answer"')" >&2; exit 1; }
echo "telegram: sent ($MODE $TAG $ACTION)"
