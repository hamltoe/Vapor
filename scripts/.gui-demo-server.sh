#!/usr/bin/env bash
# Throwaway server for manual GUI testing: publishes a few fake Windows games
# and serves them until stopped. Not part of any automated test.
set -uo pipefail
port="${1:-8896}"
action="${2:-start}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="${root}/build-linux/bin"
work="/tmp/vapor-gui-demo"

publish() {
    local id="$1" name="$2" ver="$3" dev="$4" desc="$5" mb="${6:-1}"
    local src="${work}/src/${id}"
    mkdir -p "${src}/bin" "${src}/data"
    printf '@echo off\r\necho %s running\r\necho   data=%%GAME_DATA%%\r\ntimeout /t 2 >nul\r\nexit /b 0\r\n' \
        "${name}" > "${src}/bin/game.bat"
    # Incompressible bytes, so the packaged size is roughly the size asked for
    # and a download takes long enough to watch the progress bar move.
    head -c "$((mb * 1024 * 1024))" /dev/urandom > "${src}/data/assets.pak"
    "${bin}/vapor-admin" -r "${work}/content" -d "${work}/vapor.db" \
        add "${src}" --id "${id}" --name "${name}" --version "${ver}" \
        --developer "${dev}" --description "${desc}" \
        --windows-exec 'bin/game.bat' --linux-exec 'bin/game.bat' \
        --env 'GAME_DATA=$INSTALL_DIR/data/assets.pak' > /dev/null || return 1
}

case "${action}" in
start)
    rm -rf "${work}"; mkdir -p "${work}"
    publish hollow-vale   "Hollow Vale"      1.0.3  "Smoke Works"   "A quiet descent into a flooded valley."   1
    publish neon-drifter  "Neon Drifter"     2.1.0  "Blue Circuit"  "Arcade racing through rain-slicked streets." 1
    publish stone-archive "The Stone Archive" 0.9.1 "Ledger Games"  "A puzzle game about cataloguing ruins."   2
    publish tidal-lock    "Tidal Lock"       1.4.2  "Orbit Nine"    "Survive on a world that never turns."     220
    # A second version, so the GUI has an update to offer.
    publish hollow-vale   "Hollow Vale"      1.0.10 "Smoke Works"   "A quiet descent into a flooded valley."   1

    setsid "${bin}/vapord" -p "${port}" -H 0.0.0.0 \
        -r "${work}/content" -d "${work}/vapor.db" \
        > "${work}/vapord.log" 2>&1 &
    echo $! > "${work}/vapord.pid"
    for _ in $(seq 1 50); do
        grep -q 'listening on' "${work}/vapord.log" 2>/dev/null && break
        sleep 0.1
    done
    grep -q 'listening on' "${work}/vapord.log" || { cat "${work}/vapord.log"; exit 1; }
    echo "started on port ${port}"
    ;;
stop)
    [ -f "${work}/vapord.pid" ] && kill "$(cat "${work}/vapord.pid")" 2>/dev/null
    rm -rf "${work}"
    echo stopped
    ;;
esac
