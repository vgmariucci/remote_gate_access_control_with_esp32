#!/usr/bin/env bash
#
# Every logging tag in the firmware must appear in app_main's allow-list.
#
# A production image starts with every tag at WARN (sdkconfig.prod), and
# app_main raises the project's own tags back to INFO. A component added
# later with a tag nobody listed is therefore silent on a door - and
# silent in a way nobody notices until the night it matters.
#
# This turns that from a thing to remember into a thing CI says out loud.
set -euo pipefail

cd "$(dirname "$0")/.."

ALLOWLIST=firmware/main/app_main.c
BEGIN='log-allowlist:begin'
END='log-allowlist:end'

if ! grep -q "$BEGIN" "$ALLOWLIST"; then
    echo "error: $ALLOWLIST has no '$BEGIN' marker; the allow-list cannot be checked." >&2
    exit 1
fi

block=$(sed -n "/$BEGIN/,/$END/p" "$ALLOWLIST")

tags=$(grep -rh 'static const char \*TAG' firmware/components firmware/main \
       | sed -n 's/.*TAG[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
       | sort -u)

if [ -z "$tags" ]; then
    echo "error: found no logging tags at all; the pattern has probably drifted." >&2
    exit 1
fi

missing=0
for t in $tags; do
    if ! printf '%s\n' "$block" | grep -q "\"$t\""; then
        echo "missing from the allow-list: \"$t\"" >&2
        missing=1
    fi
done

if [ "$missing" -ne 0 ]; then
    cat >&2 <<'EOF'

Add it to the block between the markers in firmware/main/app_main.c, or
decide deliberately that it should be quiet on a door and add it to the
block with a comment saying so.
EOF
    exit 1
fi

# A tag listed but no longer used is harmless - esp_log_level_set on an
# unknown tag does nothing - so this is a note, not a failure.
listed_tags=$(printf '%s\n' "$block" | grep -o '"[a-z][a-z0-9._]*"' | tr -d '"' | sort -u)
for listed in $listed_tags; do
    if ! printf '%s\n' "$tags" | grep -qx "$listed"; then
        echo "note: \"$listed\" is in the allow-list but no component uses it"
    fi
done

echo "log allow-list covers all $(printf '%s\n' "$tags" | wc -l) tag(s)"
