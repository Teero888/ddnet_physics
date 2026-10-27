#!/bin/sh
# Build the oracle of the Unique backends: a patched Unique Race server
# (https://github.com/unique-clan/unique-race) that replays input scripts.
#
# Usage: tests/unique/oracle/build.sh [unique-race checkout] [build dir]
#
# The checkout is cloned locally first, the original is never modified.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
src=${1:-$root/unique-race}
out=${2:-$root/build/unique_oracle}

mkdir -p "$out"
if [ ! -d "$out/unique-race" ]; then
  git clone --quiet --local --no-hardlinks "$src" "$out/unique-race"
  python3 "$here/patch_unique.py" "$out/unique-race"
fi
mkdir -p "$out/unique-race/src/oracle"
cp -u "$here/oracle.h" "$out/unique-race/src/oracle/oracle.h"
cp -u "$here/record.h" "$out/unique-race/src/oracle/oracle_record.h"

# -fno-lifetime-dse: see tests/oracle/build.sh.
cmake -S "$out/unique-race" -B "$out/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-fno-lifetime-dse \
  -DCLIENT=OFF -DTOOLS=OFF -DMYSQL=OFF -DAUTOUPDATE=OFF -DVIDEORECORDER=OFF \
  -DWEBSOCKETS=OFF -DANTIBOT=OFF -DUPNP=OFF -DDOWNLOAD_GTEST=OFF -DDEV=ON \
  -DPREFER_BUNDLED_LIBS=OFF
cmake --build "$out/build" --target game-server
echo "oracle: $out/build/DDNet-Server"
