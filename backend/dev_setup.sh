#!/usr/bin/env bash
#
# One-time setup for a development database, and again after every
# `docker compose down -v` - that wipes the volume, and the volume is
# the whole Postgres cluster, so the role and the marker go with it.
#
#   ./backend/dev_setup.sh
#
# Prints the DATABASE_URL line to paste into docker/.env.
set -euo pipefail

cd "$(dirname "$0")/.."

COMPOSE=docker/compose.yaml
DB_USER=${DB_USER:-gate}
DB_NAME=${DB_NAME:-gate}
API_PASS=${API_PASS:-$(openssl rand -hex 24)}

# Hex on purpose. DATABASE_URL is a URL, so a password containing @ : /
# or ? breaks the parse and produces a connection error that looks like
# anything but a quoting problem.

psql() { docker compose -f "$COMPOSE" exec -T db psql -U "$DB_USER" -d "$DB_NAME" "$@"; }

if ! psql -tAc 'select 1' >/dev/null 2>&1; then
    echo "the database container is not answering. start it first:" >&2
    echo "    docker compose -f $COMPOSE up -d db" >&2
    exit 1
fi

psql -v ON_ERROR_STOP=1 -q <<SQL
do \$\$
begin
    if not exists (select 1 from pg_roles where rolname = 'api_user') then
        create role api_user login password '${API_PASS}';
    else
        alter role api_user password '${API_PASS}';
    end if;
end \$\$;

grant gate_app to api_user;

-- The marker the test suites look for. It lives in the database rather
-- than in an environment variable so that it travels with the database
-- instead of with whoever's shell happens to be open: point
-- DATABASE_URL at production by mistake and the suites stop, because
-- production was never marked.
alter database ${DB_NAME} set gate.disposable = 'on';
SQL

URL="postgresql://api_user:${API_PASS}@localhost:5432/${DB_NAME}"
ENV_FILE=docker/.env

# Write the file rather than print the password. A secret echoed to a
# terminal ends up in scrollback, in screenshots and in chat logs - and
# the "I forgot to paste the new line" failure looks like an
# authentication bug rather than a missed step.
if [ -f "$ENV_FILE" ]; then
    if grep -q '^DATABASE_URL=' "$ENV_FILE"; then
        tmp=$(mktemp)
        # The password is hex, so no separator in it can confuse this.
        sed "s|^DATABASE_URL=.*|DATABASE_URL=${URL}|" "$ENV_FILE" > "$tmp"
        cat "$tmp" > "$ENV_FILE"      # keep the inode, and the permissions
        rm -f "$tmp"
    else
        printf '\nDATABASE_URL=%s\n' "$URL" >> "$ENV_FILE"
    fi
    echo
    echo "api_user ready, ${DB_NAME} is marked disposable,"
    echo "and ${ENV_FILE} now has the matching DATABASE_URL."
    echo "The password was not printed. Re-source it:"
    echo
    echo "    set -a; . ${ENV_FILE}; set +a"
else
    echo
    echo "api_user ready, and ${DB_NAME} is marked disposable."
    echo "There is no ${ENV_FILE} yet. Create it from .env.example, then"
    echo "run this again and it will fill in DATABASE_URL for you."
fi