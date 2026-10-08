#!/bin/sh
# Build the optimized backend with profile guided optimization into build/pgo:
# an instrumented build runs the benchmark, and is built again with the profile
# it recorded. The profile is recorded on the run through Aip-Gores
# (bench --tas) and on random inputs on the maps given, with each of the
# numbers of players given: "1 2:0.1 8:0.1" by default, which is 1 player for
# the seconds per map and 2 and 8 players for 0.1 seconds each. One tee has a
# way of its own through the tick, which more tees do not take: trained on
# 1 player only (-p 1) the build was 0.4 % faster with one tee and 10 % slower
# with two or eight; with as much time for each number of players, 1 % slower
# with one tee and 1 % faster with more.
#
# Usage: pgo.sh [-s seconds per map] [-r seconds of the run] [-p "players[:seconds]..."] [-c gcc|clang]
#        <map>...
#
# GCC is the default: with this profile it was 4 % faster on the run and 9 %
# with random inputs than without one (Clang with a profile was slower than GCC
# without one). The results of the physics do not depend on it: check with
#   DDNET_PHYSICS_OPTIMIZED_BUILD=build/pgo tests/compare_backends.sh <map>...
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
seconds=0.3
run_seconds=3
compiler=gcc
players="1 2:0.1 8:0.1"
while getopts s:r:p:c: o; do
  case $o in
  s) seconds=$OPTARG ;;
  r) run_seconds=$OPTARG ;;
  p) players=$OPTARG ;;
  c) compiler=$OPTARG ;;
  *) exit 2 ;;
  esac
done
shift $((OPTIND - 1))
[ $# -gt 0 ] || { echo "usage: $0 [-s seconds per map] [-r seconds of the run] [-p \"players[:seconds]...\"] [-c gcc|clang] <map>..." >&2; exit 2; }

# One build directory for both steps: GCC finds the profile of an object by
# the path of the object.
out=$root/build/pgo
rm -rf "$out"
if [ "$compiler" = clang ]; then
  profile=$out/ddnet_physics.profdata
  cmake -S "$root" -B "$out" -G Ninja -DCMAKE_C_COMPILER=clang -DDDNET_PHYSICS_BACKEND=optimized \
    -DDDNET_PHYSICS_NATIVE=ON -DDDNET_PHYSICS_PGO=generate > /dev/null
else
  profile=$out/profile
  cmake -S "$root" -B "$out" -G Ninja -DDDNET_PHYSICS_BACKEND=optimized -DDDNET_PHYSICS_NATIVE=ON \
    -DDDNET_PHYSICS_PGO=generate -DDDNET_PHYSICS_PGO_PROFILE="$profile" > /dev/null
fi
cmake --build "$out" --target bench > /dev/null

# the workload: random inputs on every map, and the run (3 seconds by default, against 0.3 for each map and
# number of players)
n=0
for map in "$@"; do
  for p in $players; do
    n=$((n + 1))
    s=$seconds
    case $p in *:*) s=${p#*:} p=${p%%:*} ;; esac
    LLVM_PROFILE_FILE="$out/$n.profraw" "$out/tests/bench" "$map" "$p" "$s" > /dev/null
  done
done
LLVM_PROFILE_FILE="$out/run.profraw" "$out/tests/bench" --tas 1 "$run_seconds" > /dev/null
if [ "$compiler" = clang ]; then
  llvm-profdata merge -o "$profile" "$out"/*.profraw
fi

cmake -S "$root" -B "$out" -DDDNET_PHYSICS_PGO=use -DDDNET_PHYSICS_PGO_PROFILE="$profile" > /dev/null
cmake --build "$out" > /dev/null
echo "built $out"
