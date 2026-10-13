#!/bin/bash
# Runs once, when the database is first created. Adds the game servers listed
# in AUTHORISED_SERVERS (comma-separated IPs or x.x.x.x/nn subnets) to the
# `servers` table so they can submit round stats.
set -e

IFS=',' read -ra entries <<< "${AUTHORISED_SERVERS:-}"
for entry in "${entries[@]}"; do
    entry="$(echo "$entry" | tr -d '[:space:]')"
    [ -z "$entry" ] && continue
    if ! [[ "$entry" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}(/[0-9]{1,2})?$ ]]; then
        echo "Skipping invalid AUTHORISED_SERVERS entry: $entry" >&2
        continue
    fi
    echo "Authorising game server: $entry"
    mariadb -uroot -p"$MARIADB_ROOT_PASSWORD" "$MARIADB_DATABASE" -e \
        "INSERT INTO servers (ip, authorised, prefix) VALUES ('$entry', 1, 'BF2142');"
done
