#!/usr/bin/env bash
# Installs everything Vapor needs to build on a Debian or Ubuntu machine.
#
# Only three things come from the package manager - libcurl for the client,
# libsodium for the server, and SDL2 for the host/client windows. Everything
# else is vendored under third_party/ by scripts/vendor-deps.sh.
#
# Usage: scripts/bootstrap-linux.sh [--server-only] [--client-only]
set -euo pipefail

want_gui=1
want_server=1
want_client=1

for arg in "$@"; do
    case "${arg}" in
    --server-only) want_client=0 ;;
    --client-only) want_server=0 ;;
    -h|--help)
        sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "bootstrap-linux: unknown option ${arg}" >&2
        exit 1
        ;;
    esac
done

if ! command -v apt-get >/dev/null 2>&1; then
    cat >&2 <<'EOF'
bootstrap-linux: this script only knows apt. On another distribution install the
equivalents of: build-essential, cmake, pkg-config, libcurl4-openssl-dev,
libsodium-dev, libsdl2-dev, libgl1-mesa-dev.
EOF
    exit 1
fi

pkgs=(build-essential cmake pkg-config ca-certificates curl)
[ "${want_client}" -eq 1 ] && pkgs+=(libcurl4-openssl-dev)
[ "${want_server}" -eq 1 ] && pkgs+=(libsodium-dev)
[ "${want_gui}" -eq 1 ] && pkgs+=(libsdl2-dev libgl1-mesa-dev)

sudo=""
if [ "$(id -u)" -ne 0 ]; then
    sudo="sudo"
fi

echo "installing: ${pkgs[*]}"
${sudo} apt-get update
${sudo} apt-get install -y "${pkgs[@]}"

echo
echo "done. next:"
echo "  scripts/vendor-deps.sh     fetch the vendored third_party sources"
echo "  scripts/build-linux.sh     configure and build"
