#!/bin/sh
# Compare ddnet_physics against the oracle on a list of maps, with a number of
# server configurations per map.
#
# Usage: run_suite.sh [-j jobs] [-t ticks] [-p players] <map>...
#
# Prints one line per run and exits non-zero if any run differs.
set -u
here=$(cd "$(dirname "$0")" && pwd)
jobs=$(nproc 2>/dev/null || echo 4) ticks=4000 players=8
while getopts j:t:p: opt; do
  case $opt in
  j) jobs=$OPTARG ;;
  t) ticks=$OPTARG ;;
  p) players=$OPTARG ;;
  *) exit 2 ;;
  esac
done
shift $((OPTIND - 1))

# generator options -- server settings
configs='--kills
--kills --teams
--kills --teams --spec
--kills --teams --spec -- sv_pauseable 1
--kills --spec -- sv_pauseable 1 sv_pause_frequency 0 sv_no_weak_hook 1
--kills --spec -- sv_team 3 sv_pauseable 1
--kills -- sv_solo_server 1
--kills --teams -- sv_no_weak_hook 1
--kills --teams -- sv_team 2 sv_rejoin_team_0 0 sv_max_team_size 2
--kills -- sv_old_teleport_hook 1 sv_old_teleport_weapons 1 sv_old_laser 1
--kills --teams -- sv_teleport_hold_hook 1 sv_teleport_lose_weapons 1 sv_destroy_lasers_on_death 1 sv_destroy_bullets_on_death 0
--kills -- sv_hit 0 sv_endless_drag 1 sv_deepfly 0 sv_freeze_delay 1 sv_reset_pickups 1
--kills --teams --spec -- sv_plasma_per_sec 10 sv_plasma_range 2000 sv_dragger_range 2000'

work=$(mktemp -d "${TMPDIR:-/tmp}/ddnet_suite.XXXXXX")
trap 'rm -rf "$work"' EXIT

seed=0
for map in "$@"; do
  echo "$configs" | while IFS= read -r config; do
    seed=$((seed + 1))
    printf '%s\t%s\t%s\n' "$map" "$(cksum < "$map" | cut -d' ' -f1)$seed" "$config"
  done
done > "$work/runs"

export here players ticks
# One compare.sh per line, $jobs at a time.
tr '\t' '\037' < "$work/runs" | xargs -P "$jobs" -I{} sh -c '
  line=$1
  map=$(printf "%s" "$line" | cut -d "$(printf "\037")" -f1)
  seed=$(printf "%s" "$line" | cut -d "$(printf "\037")" -f2)
  config=$(printf "%s" "$line" | cut -d "$(printf "\037")" -f3)
  result=$("$here/compare.sh" "$map" "$players" "$ticks" "$((seed % 100000))" $config 2>&1 | tail -4 | tr "\n" " ")
  case $result in identical*) status=ok ;; *) status=FAIL ;; esac
  printf "%s\t%s\t%s\t%s\n" "$status" "$(basename "$map")" "$config" "$result"
' sh {} > "$work/results"

sort "$work/results"
total=$(wc -l < "$work/results")
failed=$(grep -c '^FAIL' "$work/results")
echo "$((total - failed))/$total runs identical"
[ "$failed" -eq 0 ]
