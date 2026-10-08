#!/bin/sh
# Run one input script through both the oracle and ddnet_physics and compare.
#
# Usage: compare.sh <map> <players> <ticks> <seed> [gen_script.py options] [-- setting value ...]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
bin=${DDNET_PHYSICS_BUILD:-$root/build/release}/tests

map=$1 players=$2 ticks=$3 seed=$4
shift 4
gen_options=""
while [ $# -gt 0 ] && [ "$1" != "--" ]; do
  gen_options="$gen_options $1"
  shift
done
[ $# -gt 0 ] && shift

work=$(mktemp -d "${TMPDIR:-/tmp}/ddnet_compare.XXXXXX")
trap 'rm -rf "$work"' EXIT

# shellcheck disable=SC2086
"$bin/map_points" "$map" > "$work/points"
python3 "$here/gen_script.py" "$work/script" "$players" "$ticks" "$seed" --teleports "$work/points" $gen_options
"$here/run_oracle.sh" "$map" "$work/script" "$work/expected" "$@"
"$bin/replay" "$map" "$work/script" "$work/actual" "$@"
"$bin/dump_diff" "$work/expected" "$work/actual"
