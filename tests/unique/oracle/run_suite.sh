#!/bin/sh
# Compare a Unique backend against the oracle on a list of maps, in every game
# mode a map is played in (race maps in race, with and without
# sv_health_and_ammo and sv_kill_grenades; fastcap maps with and without
# weapons).
#
# Usage: run_suite.sh [-j jobs] [-t ticks] [-p players] [-n seeds] <map>...
#
# A map is a fastcap map if its name has "ctf" in it or a <map>.cfg next to it
# says "sv_gametype fastcap" (as in unique-maps). Prints one line per run and
# exits non-zero if any run differs.
set -u
here=$(cd "$(dirname "$0")" && pwd)
jobs=$(nproc 2>/dev/null || echo 4) ticks=3000 players=8 seeds=1
while getopts j:t:p:n: opt; do
  case $opt in
  j) jobs=$OPTARG ;;
  t) ticks=$OPTARG ;;
  p) players=$OPTARG ;;
  n) seeds=$OPTARG ;;
  *) exit 2 ;;
  esac
done
shift $((OPTIND - 1))

# mode generator options -- server settings
race_configs='race --kills
race --kills -- sv_health_and_ammo 1
race --kills -- sv_kill_grenades 1
race --kills -- sv_health_and_ammo 1 sv_kill_grenades 1'
fastcap_configs='fastcap --kills --sides
fastcap_no_weapons --kills --sides'

work=$(mktemp -d "${TMPDIR:-/tmp}/unique_suite.XXXXXX")
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

export here players ticks
xargs -P "$jobs" -I{} sh -c '
  line=$1
  map=$(printf "%s" "$line" | cut -d "$(printf "\037")" -f1)
  seed=$(printf "%s" "$line" | cut -d "$(printf "\037")" -f2)
  config=$(printf "%s" "$line" | cut -d "$(printf "\037")" -f3)
  mode=${config%% *}
  rest=${config#* }
  result=$("$here/compare.sh" "$map" "$players" "$ticks" "$seed" "$mode" $rest 2>&1 | tail -4 | tr "\n" " ")
  case $result in identical*) status=ok ;; *) status=FAIL ;; esac
  printf "%s\t%s\t%s\t%s\t%s\n" "$status" "$(basename "$map")" "$seed" "$config" "$result"
' sh {} < "$work/runs" > "$work/results"

grep '^FAIL' "$work/results" | sort | head -40
total=$(wc -l < "$work/results")
failed=$(grep -c '^FAIL' "$work/results")
echo "$((total - failed))/$total runs identical"
[ "$failed" -eq 0 ]
