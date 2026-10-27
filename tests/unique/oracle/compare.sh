#!/bin/sh
# Run one input script through both the Unique oracle and ddnet_physics and
# compare.
#
# Usage: compare.sh <map> <players> <ticks> <seed> <race|fastcap|fastcap_no_weapons>
#                   [gen_script.py options] [-- setting value ...]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
bin=${DDNET_PHYSICS_BUILD:-$root/build/unique_reference}/tests/unique

map=$1 players=$2 ticks=$3 seed=$4 mode=$5
shift 5
gen_options=""
while [ $# -gt 0 ] && [ "$1" != "--" ]; do
  gen_options="$gen_options $1"
  shift
done
[ $# -gt 0 ] && shift

case $mode in
race) gametype="race" ;;
fastcap) gametype="fastcap" ;;
fastcap_no_weapons) gametype="fastcap sv_no_weapons 1" ;;
*) echo "unknown mode $mode" >&2; exit 2 ;;
esac

work=$(mktemp -d "${TMPDIR:-/tmp}/unique_compare.XXXXXX")
trap 'rm -rf "$work"' EXIT

"$bin/map_points" "$map" > "$work/points"
# shellcheck disable=SC2086
python3 "$here/gen_script.py" "$work/script" "$players" "$ticks" "$seed" --teleports "$work/points" $gen_options
# shellcheck disable=SC2086
"$here/run_oracle.sh" "$map" "$work/script" "$work/expected" $gametype "$@"
"$bin/replay" "$map" "$work/script" "$work/actual" "$mode" "$@"
"$bin/dump_diff" "$work/expected" "$work/actual"
