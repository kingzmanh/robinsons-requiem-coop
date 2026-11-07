#!/bin/sh
# Build the release installer: clean GUI engine build (no game art, default icon), stage, makensis.
#   sh installer/build.sh [version]
# Output: release/RRQ-Native-Setup-<version>.exe
set -e
VERSION=${1:-0.1.0}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
export PATH=/c/msys64/mingw64/bin:$PATH
JOBS=${JOBS:-8}

B="$ROOT/build/engine-release"
cmake -S "$ROOT/engine" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DRRQ_GUI=ON -DRRQ_ICON= > /dev/null
cmake --build "$B" -j "$JOBS"

S="$ROOT/installer/stage"
rm -rf "${S:?}"
mkdir -p "$S" "$ROOT/release"
cp "$B/alis.exe" "$S/RRQ.exe"
strip "$S/RRQ.exe"
cp /c/msys64/mingw64/bin/SDL2.dll "$S/SDL2.dll"
cp "$ROOT/installer/README.txt" "$S/README.txt"
cp "$ROOT/engine/LICENSE" "$S/LICENSE-alis.txt"
cp /c/msys64/mingw64/share/licenses/SDL2/LICENSE.txt "$S/LICENSE-SDL2.txt"

cd "$ROOT/installer"
makensis -V2 -DVERSION="$VERSION" RRQ-Native.nsi
ls -la "$ROOT/release/RRQ-Native-Setup-$VERSION.exe"
