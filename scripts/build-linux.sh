#!/usr/bin/env bash
# Configure and build on Linux. Extra CMake flags are passed through.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${root}/build-linux"

if [ ! -f "${root}/third_party/sqlite/sqlite3.c" ]; then
    echo "third_party/ is empty; run scripts/vendor-deps.sh first" >&2
    exit 1
fi

# The GUI is required: vapord's host window and vapor-gui both use SDL2.
if ! pkg-config --exists sdl2 gl 2>/dev/null; then
    echo "SDL2/OpenGL not found; run: sudo bash scripts/bootstrap-linux.sh" >&2
    exit 1
fi

cmake -S "${root}" -B "${build}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}" \
      -DVAPOR_BUILD_GUI=ON "$@"
cmake --build "${build}" -j "$(nproc)"

echo
echo "binaries in ${build}/bin:"
ls -1 "${build}/bin"
