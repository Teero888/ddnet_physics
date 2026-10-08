#!/bin/sh
# Check the optimized backend against the reference backend: replay random
# input scripts with both and compare the state of every tick bit for bit, and
# what is drawn of it. The optimized backend runs twice, with its build without
# events and with its event build, which also has to make the same effects and
# sounds (DDNET_PHYSICS_NO_EVENTS=1 leaves that run out).
#
# Usage: compare_backends.sh [-j jobs] [-t ticks] [-p players] [-n seeds] <map>...
#
# Needs both builds:
#   cmake -S . -B build/release                                     (reference)
#   cmake -S . -B build/opt -DDDNET_PHYSICS_BACKEND=optimized
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
ref=${DDNET_PHYSICS_REFERENCE_BUILD:-$root/build/release}/tests
opt=${DDNET_PHYSICS_OPTIMIZED_BUILD:-$root/build/opt}/tests
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

work=$(mktemp -d "${TMPDIR:-/tmp}/ddnet_backends.XXXXXX")
trap 'rm -rf "$work"' EXIT

n=0
for map in "$@"; do
  s=0
  while [ "$s" -lt "$seeds" ]; do
    s=$((s + 1))
    echo "$configs" | while IFS= read -r config; do
      n=$((n + 1))
      printf '%s\037%s\037%s\n' "$map" "$(($(cksum < "$map" | cut -d' ' -f1) % 10000 + n + s * 1000))" "$config"
    done
    n=$((n + 13))
  done
done > "$work/runs"

export here ref opt players ticks work
xargs -P "$jobs" -I{} sh -c '
  line=$1
  map=$(printf "%s" "$line" | cut -d "$(printf "\037")" -f1)
  seed=$(printf "%s" "$line" | cut -d "$(printf "\037")" -f2)
  config=$(printf "%s" "$line" | cut -d "$(printf "\037")" -f3)
  gen=${config%%--*}; case $config in *" -- "*) settings=${config#* -- } ;; *) settings= ;; esac
  gen=$(printf "%s" "$config" | sed "s/ -- .*//")
  d=$(mktemp -d "$work/run.XXXXXX")
  "$ref/map_points" "$map" > "$d/points"
  python3 "$here/oracle/gen_script.py" "$d/script" "$players" "$ticks" "$seed" --teleports "$d/points" $gen
  REPLAY_EVENTS="$d/expected_events" REPLAY_DRAW="$d/expected_draw" "$ref/replay" "$map" "$d/script" "$d/expected" $settings >/dev/null 2>&1
  REPLAY_DRAW="$d/actual_draw" "$opt/replay" "$map" "$d/script" "$d/actual" $settings >/dev/null 2>&1
  result=$("$ref/dump_diff" "$d/expected" "$d/actual" 2>&1 | tail -4 | tr "\n" " ")
  case $result in identical*) status=ok ;; *) status=FAIL ;; esac
  if [ "$status" = ok ] && ! cmp -s "$d/expected_draw" "$d/actual_draw"; then
    status=FAIL result="drawing differs: $(cmp "$d/expected_draw" "$d/actual_draw" 2>&1)"
  fi
  # the event build: the same, and the effects and sounds
  if [ "$status" = ok ] && [ "${DDNET_PHYSICS_NO_EVENTS:-0}" != 1 ]; then
    REPLAY_EVENTS="$d/actual_events" REPLAY_DRAW="$d/actual_draw" "$opt/replay" "$map" "$d/script" "$d/actual" $settings >/dev/null 2>&1
    events=$("$ref/dump_diff" "$d/expected" "$d/actual" 2>&1 | tail -4 | tr "\n" " ")
    case $events in
    identical*)
      if ! cmp -s "$d/expected_draw" "$d/actual_draw"; then
        events="event build: drawing differs: $(cmp "$d/expected_draw" "$d/actual_draw" 2>&1)"
      else
        events=$(python3 "$here/event_diff.py" "$d/expected_events" "$d/actual_events" 2>&1 | head -4 | tr "\n" " ")
      fi ;;
    *) events="event build: $events" ;;
    esac
    case $events in identical*) result="$result, events $events" ;; *) status=FAIL result="$events" ;; esac
  fi
  printf "%s\t%s\t%s\t%s\t%s\n" "$status" "$(basename "$map")" "$seed" "$config" "$result"
  rm -rf "$d"
' sh {} < "$work/runs" > "$work/results"

grep '^FAIL' "$work/results" | sort | head -20
total=$(wc -l < "$work/results")
failed=$(grep -c '^FAIL' "$work/results")
echo "$((total - failed))/$total runs identical"
[ "$failed" -eq 0 ]
