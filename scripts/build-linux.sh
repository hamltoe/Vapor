#!/usr/bin/env bash
# Configure and build on Linux. Pass extra CMake flags as arguments, e.g.
#   scripts/build-linux.sh -DVAPOR_BUILD_GUI=ON
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${root}/build-linux"

if [ ! -f "${root}/third_party/sqlite/sqlite3.c" ]; then
    echo "third_party/ is empty; run scripts/vendor-deps.sh first" >&2
    exit 1
fi

# The GUI needs SDL2 and OpenGL from the system, so it is built only when they
# are actually present. An explicit -DVAPOR_BUILD_GUI on the command line wins.
gui=OFF
if pkg-config --exists sdl2 gl 2>/dev/null; then
    gui=ON
fi
case " $* " in
*" -DVAPOR_BUILD_GUI"*) gui="" ;;
esac

cmake -S "${root}" -B "${build}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}" \
      ${gui:+-DVAPOR_BUILD_GUI=${gui}} "$@"
cmake --build "${build}" -j "$(nproc)"

echo
echo "binaries in ${build}/bin:"
ls -1 "${build}/bin"
