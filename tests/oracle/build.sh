#!/bin/sh
# Build the oracle: a patched DDNet-Server that replays input scripts.
#
# Usage: tests/oracle/build.sh [ddnet checkout] [build dir]
#
# The checkout is cloned locally first, the original is never modified.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
src=${1:-$root/ddnet}
out=${2:-$root/build/oracle}

mkdir -p "$out"
if [ ! -d "$out/ddnet" ]; then
  git clone --quiet --local --no-hardlinks "$src" "$out/ddnet"
  python3 "$here/patch_ddnet.py" "$out/ddnet"
fi
mkdir -p "$out/ddnet/src/oracle"
cp -u "$here/oracle.h" "$out/ddnet/src/oracle/oracle.h"
cp -u "$here/record.h" "$out/ddnet/src/oracle/oracle_record.h"

# DDNet's entities rely on their operator new zeroing the memory: CLight never
# initializes m_Speed, m_LengthL, m_CurveLength and m_AngularSpeed and reads
# them in its constructor. GCC removes that zeroing as a dead store
# (-flifetime-dse), which leaves those members with whatever was on the heap
# and makes laser walls behave differently from run to run. The oracle is built
# with the zeroing kept, which is the behavior the code intends.
cmake -S "$out/ddnet" -B "$out/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-fno-lifetime-dse \
  -DCLIENT=OFF -DTOOLS=OFF -DMYSQL=OFF -DAUTOUPDATE=OFF -DVIDEORECORDER=OFF \
  -DWEBSOCKETS=OFF -DANTIBOT=OFF -DUPNP=OFF -DDOWNLOAD_GTEST=OFF -DDEV=ON \
  -DPREFER_BUNDLED_LIBS=OFF
cmake --build "$out/build" --target game-server
echo "oracle: $out/build/DDNet-Server"
