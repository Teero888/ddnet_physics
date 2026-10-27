#!/bin/sh
# Run the oracle (the patched Unique Race server) on a map with an input script.
#
# Usage: run_oracle.sh <map> <script> <dump> <gametype> [setting value]...
#
# gametype is the sv_gametype of the server: race or fastcap (with
# "sv_no_weapons 1" for fastcap without weapons).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
oracle=${UNIQUE_ORACLE:-$root/build/unique_oracle/build}

map=$(realpath "$1")
script=$(realpath "$2")
dump=$(realpath -m "$3")
gametype=$4
shift 4

# A private storage directory, so that no configuration of the user is read.
run=$(mktemp -d "${TMPDIR:-/tmp}/unique_oracle.XXXXXX")
trap 'rm -rf "$run"' EXIT
mkdir "$run/maps"
cp "$map" "$run/maps/oracle.map"
ln -s "$oracle/data" "$run/data"
printf 'add_path $CURRENTDIR\nadd_path $DATADIR\n' > "$run/storage.cfg"
# Shadow the example configuration of the data directory: plain defaults only.
: > "$run/autoexec_server.cfg"
: > "$run/reset.cfg"

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
  "logfile server.log" "sv_max_clients 64" "sv_ddrace_tune_reset 0" "sv_gametype $gametype" "$settings" \
  "sv_map oracle" \
  > "$run/stdout.log" 2>&1 || { tail -30 "$run/stdout.log" >&2; exit 1; }
