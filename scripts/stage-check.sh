#!/usr/bin/env bash
# Temporary milestone check: packaging and catalog. Superseded by smoke-test.sh.
set -uo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="${root}/build-linux/bin"
work="$(mktemp -d /tmp/vapor-stage-XXXXXX)"
port=8897
trap 'kill $pid 2>/dev/null; rm -rf "$work"' EXIT

export XDG_DATA_HOME="${work}/cd" HOME="${work}/home"
mkdir -p "$XDG_DATA_HOME" "$HOME"

"${bin}/vapord" -p $port -r "${work}/content" -d "${work}/v.db" > "${work}/log" 2>&1 &
pid=$!
for _ in $(seq 1 50); do grep -q listening "${work}/log" && break; sleep 0.1; done

V="${bin}/vapor"
A="${bin}/vapor-admin -r ${work}/content -d ${work}/v.db"

# Build a fake game tree, including an empty dir and a nested lib.
g="${work}/src/hollow-vale"
mkdir -p "$g/bin" "$g/lib" "$g/saves"
printf '#!/usr/bin/env bash\necho "Hollow Vale running"\necho "cwd=$(pwd)"\necho "data=${HOLLOW_DATA:-unset}"\n' > "$g/bin/hollowvale"
echo 'so' > "$g/lib/libhollow.so.1"
echo 'assets' > "$g/assets.dat"

echo "=== admin add 1.0.3 ==="
$A add "$g" --id hollow-vale --name "Hollow Vale" --version 1.0.3 \
    --developer "Smoke Works" --description "A test game." \
    --linux-exec bin/hollowvale --exec-bit 'bin/hollowvale' --exec-bit 'lib/*.so*' \
    --env 'HOLLOW_DATA=$INSTALL_DIR/assets.dat' --windows-exec 'bin/HollowVale.exe'

echo "=== admin add 1.0.10 (should sort above 1.0.3) ==="
$A add "$g" --id hollow-vale --name "Hollow Vale" --version 1.0.10 \
    --linux-exec bin/hollowvale --exec-bit 'bin/hollowvale'

echo "=== invalid id rejected ==="
$A add "$g" --id 'BAD ID' --name x --version 1 --linux-exec bin/hollowvale; echo "exit=$?"

echo "=== admin list ==="
$A list

echo "=== generated manifest ==="
cat "${work}/content/hollow-vale/1.0.3/manifest.json"

echo "=== content tree ==="
find "${work}/content" -type f | sed "s|${work}/content|.|"

echo "=== client login ==="
$V config server "http://127.0.0.1:${port}" > /dev/null
$V config library "${work}/games" > /dev/null
VAPOR_PASSWORD=hunter2hunter2 $V register alice > /dev/null
VAPOR_PASSWORD=hunter2hunter2 $V login alice

echo "=== vapor list ==="
$V list

echo "=== vapor info ==="
$V info hollow-vale

echo "=== info on unknown game ==="
$V info nosuchgame; echo "exit=$?"

echo "=== manifest endpoint (latest resolves server-side) ==="
TOKEN=$(sed -n 's/^token = //p' "$XDG_DATA_HOME/vapor/config.ini")
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://127.0.0.1:${port}/api/v1/games/hollow-vale/versions/latest/manifest" \
  | head -c 200
echo
echo "=== traversal attempts rejected ==="
for bad in '../../etc/passwd' '..%2f..%2fetc' 'Hollow'; do
  code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $TOKEN" \
    "http://127.0.0.1:${port}/api/v1/games/${bad}")
  echo "  games/${bad} -> HTTP ${code}"
done
echo "=== installed (should be empty) ==="
$V installed
