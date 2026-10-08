#!/bin/sh
# Run the oracle (the patched DDNet server) on a map with an input script.
#
# Usage: run_oracle.sh <map> <script> <dump> [setting value]...
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
oracle=${DDNET_ORACLE:-$root/build/oracle/build}

map=$(realpath "$1")
script=$(realpath "$2")
dump=$(realpath -m "$3")
shift 3

# A private storage directory, so that no configuration of the user is read.
run=$(mktemp -d "${TMPDIR:-/tmp}/ddnet_oracle.XXXXXX")
trap 'rm -rf "$run"' EXIT
mkdir "$run/maps"
cp "$map" "$run/maps/oracle.map"
ln -s "$oracle/data" "$run/data"
printf 'add_path $CURRENTDIR\nadd_path $DATADIR\n' > "$run/storage.cfg"
# Shadow the example configuration of the data directory: plain defaults only.
: > "$run/autoexec_server.cfg"
: > "$run/reset.cfg"

# sv_ddrace_tune_reset is turned off below: it would reset part of the settings
# given here on map load. ddnet_physics takes its config as given.
settings=""
while [ $# -ge 2 ]; do
  settings="$settings$1 $2;"
  shift 2
done

port=$((20000 + ($$ % 20000)))
cd "$run"
DDNET_ORACLE_SCRIPT="$script" DDNET_ORACLE_DUMP="$dump" \
  "$oracle/DDNet-Server" \
  "sv_register 0" "bindaddr 127.0.0.1" "sv_port $port" "sv_sqlite_file ddnet-server.sqlite" \
  "logfile server.log" "sv_max_clients 64" "sv_ddrace_tune_reset 0" "$settings" "sv_map oracle" \
  > "$run/stdout.log" 2>&1 || { tail -30 "$run/stdout.log" >&2; exit 1; }
