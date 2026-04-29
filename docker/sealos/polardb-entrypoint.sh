#!/usr/bin/env bash
set -euo pipefail

BASE=${POLARDB_HOME:-/u01/polardb_pg}
DATA_ROOT=${POLARDB_DATA_ROOT:-/var/lib/polardb}
PRIMARY=${POLARDB_PRIMARY_DIR:-$DATA_ROOT/primary}
SHARED=${POLARDB_SHARED_DIR:-$DATA_ROOT/shared}
PORT=${POLARDB_PORT:-5432}
CLUSTER_NAME=${POLARDB_CLUSTER_NAME:-primary}
INIT_MARKER="$PRIMARY/PG_VERSION"

mkdir -p "$PRIMARY" "$SHARED"
chown -R postgres:postgres "$DATA_ROOT"

run_as_postgres() {
  if [ "$(id -u)" = "0" ]; then
    runuser -u postgres -- "$@"
  else
    "$@"
  fi
}

if [ ! -s "$INIT_MARKER" ]; then
  echo "Initializing PolarDB data directories under $DATA_ROOT"

  run_as_postgres "$BASE/bin/initdb" -D "$PRIMARY" -k -A trust

  cat "$BASE/share/postgresql/polardb.conf.sample" >> "$PRIMARY/postgresql.conf"
  {
    echo "port = $PORT"
    echo "listen_addresses = '*'"
    echo "polar_datadir = 'file-dio://$SHARED'"
  } >> "$PRIMARY/postgresql.conf"

  {
    echo "host all all 0.0.0.0/0 trust"
    echo "host all all ::/0 trust"
  } >> "$PRIMARY/pg_hba.conf"

  run_as_postgres "$BASE/bin/polar-initdb.sh" "$PRIMARY/" "$SHARED/" primary localfs
  echo "PolarDB initialization completed"
else
  echo "PolarDB data directory already initialized, skipping init"
fi

if [ "$(id -u)" = "0" ]; then
  exec runuser -u postgres -- "$BASE/bin/postgres" -D "$PRIMARY" --cluster-name="$CLUSTER_NAME"
fi

exec "$BASE/bin/postgres" -D "$PRIMARY" --cluster-name="$CLUSTER_NAME"
