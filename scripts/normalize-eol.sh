#!/usr/bin/env bash
# Strips CR from files that must be LF-only. Editors on Windows will happily
# write CRLF into shell scripts and systemd units, which then fail to run.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${root}"

fixed=0
while IFS= read -r -d '' f; do
    if grep -qU $'\r' "$f" 2>/dev/null; then
        sed -i 's/\r$//' "$f"
        echo "  fixed ${f#./}"
        fixed=$((fixed + 1))
    fi
done < <(find . -path ./third_party -prune -o -path ./build-linux -prune -o \
              -type f \( -name '*.sh' -o -name '*.service' -o -name 'Caddyfile' \) \
              -print0)

echo "${fixed} file(s) normalized to LF"
