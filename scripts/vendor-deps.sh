#!/usr/bin/env bash
# Fetches vendored third-party sources into third_party/.
# These are deliberately not committed; run this once after cloning.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tp="${root}/third_party"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/vapor-vendor.XXXXXX")"
cleanup() { rm -rf "${tmp}"; }
trap cleanup EXIT

fetch() {
    local url="$1" dest="$2"
    mkdir -p "$(dirname "${dest}")"
    echo "  fetch $(basename "${dest}")"
    curl -fsSL --retry 3 --connect-timeout 30 -o "${dest}" "${url}"
}

echo 'cJSON (MIT)'
for f in cJSON.c cJSON.h; do
    fetch "https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/${f}" \
          "${tp}/cjson/${f}"
done

echo 'SQLite (public domain)'
fetch 'https://sqlite.org/2025/sqlite-amalgamation-3500400.zip' "${tmp}/sqlite.zip"
python3 - <<PY
import zipfile
z = zipfile.ZipFile("${tmp}/sqlite.zip")
z.extractall("${tmp}")
PY
mkdir -p "${tp}/sqlite"
for f in sqlite3.c sqlite3.h sqlite3ext.h; do
    cp "${tmp}/sqlite-amalgamation-3500400/${f}" "${tp}/sqlite/${f}"
done

echo 'miniz (MIT)'
fetch 'https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip' \
      "${tmp}/miniz.zip"
python3 - <<PY
import zipfile
z = zipfile.ZipFile("${tmp}/miniz.zip")
z.extractall("${tmp}/miniz")
PY
mkdir -p "${tp}/miniz"
for f in miniz.c miniz.h; do
    found="$(find "${tmp}/miniz" -name "${f}" | head -n 1)"
    if [ -z "${found}" ]; then
        echo "miniz: ${f} not found in release archive" >&2
        exit 1
    fi
    cp "${found}" "${tp}/miniz/${f}"
done

echo 'civetweb (MIT)'
fetch 'https://github.com/civetweb/civetweb/archive/refs/tags/v1.16.tar.gz' \
      "${tmp}/civetweb.tar.gz"
tar -xzf "${tmp}/civetweb.tar.gz" -C "${tmp}"
mkdir -p "${tp}/civetweb"
cp "${tmp}/civetweb-1.16/include/civetweb.h" "${tp}/civetweb/civetweb.h"
cp "${tmp}/civetweb-1.16/src/civetweb.c" "${tp}/civetweb/civetweb.c"
cp "${tmp}/civetweb-1.16/src/"*.inl "${tp}/civetweb/"

echo 'stb_image (public domain)'
fetch 'https://raw.githubusercontent.com/nothings/stb/master/stb_image.h' \
      "${tp}/stb/stb_image.h"

echo 'Nuklear (public domain)'
fetch 'https://raw.githubusercontent.com/Immediate-Mode-UI/Nuklear/master/nuklear.h' \
      "${tp}/nuklear/nuklear.h"
fetch 'https://raw.githubusercontent.com/Immediate-Mode-UI/Nuklear/master/demo/sdl_opengl2/nuklear_sdl_gl2.h' \
      "${tp}/nuklear/nuklear_sdl_gl2.h"

echo
echo "third_party/ populated at ${tp}"
