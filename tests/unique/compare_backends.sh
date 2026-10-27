#!/bin/sh
# Check the optimized Unique backend against the reference one: replay random
# input scripts with both and compare the state of every tick bit for bit, in
# every game mode a map is played in (see oracle/run_suite.sh).
#
# Usage: compare_backends.sh [-j jobs] [-t ticks] [-p players] [-n seeds] <map>...
#
# Needs both builds:
#   cmake -S . -B build/unique_reference -DDDNET_PHYSICS_BACKEND=unique_reference
#   cmake -S . -B build/unique_optimized -DDDNET_PHYSICS_BACKEND=unique_optimized
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
ref=${DDNET_PHYSICS_REFERENCE_BUILD:-$root/build/unique_reference}/tests/unique
opt=${DDNET_PHYSICS_OPTIMIZED_BUILD:-$root/build/unique_optimized}/tests/unique
jobs=$(nproc 2>/dev/null || echo 4) ticks=4000 players=8 seeds=1
while getopts j:t:p:n: o; do
  case $o in
  j) jobs=$OPTARG ;;
  t) ticks=$OPTARG ;;
  p) players=$OPTARG ;;
  n) seeds=$OPTARG ;;
  *) exit 2 ;;
  esac
done
shift $((OPTIND - 1))

race_configs='race --kills
race --kills -- sv_health_and_ammo 1
race --kills -- sv_kill_grenades 1
race --kills -- sv_health_and_ammo 1 sv_kill_grenades 1'
fastcap_configs='fastcap --kills --sides
fastcap_no_weapons --kills --sides'

work=$(mktemp -d "${TMPDIR:-/tmp}/unique_backends.XXXXXX")
trap 'rm -rf "$work"' EXIT

n=0
for map in "$@"; do
  configs=$race_configs
  if grep -qs "sv_gametype fastcap" "$map.cfg" || case $(basename "$map") in *ctf*) true ;; *) false ;; esac; then
    configs=$fastcap_configs
  fi
  s=0
  while [ "$s" -lt "$seeds" ]; do
    s=$((s + 1))
    echo "$configs" | while IFS= read -r config; do
      n=$((n + 1))
      printf '%s\037%s\037%s\n' "$map" "$(($(cksum < "$map" | cut -d' ' -f1) % 10000 + n + s * 1000))" "$config"
    done
    n=$((n + 7))
  done
done > "$work/runs"

export here ref opt players ticks work
xargs -P "$jobs" -I{} sh -c '
  line=$1
  map=$(printf "%s" "$line" | cut -d "$(printf "\037")" -f1)
  seed=$(printf "%s" "$line" | cut -d "$(printf "\037")" -f2)
  config=$(printf "%s" "$line" | cut -d "$(printf "\037")" -f3)
  mode=${config%% *}
  rest=${config#* }
  gen=$(printf "%s" "$rest" | sed "s/ -- .*//; s/^-- .*//")
  case $rest in *"-- "*) settings=${rest#*-- } ;; *) settings= ;; esac
  d=$(mktemp -d "$work/run.XXXXXX")
  "$ref/map_points" "$map" > "$d/points"
  python3 "$here/oracle/gen_script.py" "$d/script" "$players" "$ticks" "$seed" --teleports "$d/points" $gen
  "$ref/replay" "$map" "$d/script" "$d/expected" "$mode" $settings > /dev/null 2>&1
  "$opt/replay" "$map" "$d/script" "$d/actual" "$mode" $settings > /dev/null 2>&1
  result=$("$ref/dump_diff" "$d/expected" "$d/actual" 2>&1 | tail -4 | tr "\n" " ")
  case $result in identical*) status=ok ;; *) status=FAIL ;; esac
  printf "%s\t%s\t%s\t%s\t%s\n" "$status" "$(basename "$map")" "$seed" "$config" "$result"
  rm -rf "$d"
' sh {} < "$work/runs" > "$work/results"

grep '^FAIL' "$work/results" | sort | head -40
total=$(wc -l < "$work/results")
failed=$(grep -c '^FAIL' "$work/results")
echo "$((total - failed))/$total runs identical"
[ "$failed" -eq 0 ]
