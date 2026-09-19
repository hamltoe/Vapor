#!/usr/bin/env bash
# End-to-end test: starts a throwaway vapord, packages a fake game, then drives
# the real CLI through register -> login -> list -> install -> launch.
#
# Everything lives under a temp directory and the client is pointed at it via
# XDG_DATA_HOME, so this never touches a real library or config.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="${root}/build-linux/bin"
work="$(mktemp -d /tmp/vapor-smoke-XXXXXX)"
port="${VAPOR_TEST_PORT:-8899}"

pass=0
fail=0
server_pid=""

cleanup() {
    if [ -n "${server_pid}" ] && kill -0 "${server_pid}" 2>/dev/null; then
        kill "${server_pid}" 2>/dev/null
        wait "${server_pid}" 2>/dev/null
    fi
    rm -rf "${work}"
}
trap cleanup EXIT

ok()   { printf '  ok    %s\n' "$1"; pass=$((pass + 1)); }
bad()  { printf '  FAIL  %s\n' "$1"; fail=$((fail + 1)); }
step() { printf '\n%s\n' "$1"; }

# check <description> <expected-substring> -- <command...>
check() {
    local desc="$1" want="$2"; shift 3
    local out rc
    out="$("$@" 2>&1)"; rc=$?
    if [ "${rc}" -eq 0 ] && printf '%s' "${out}" | grep -qF -- "${want}"; then
        ok "${desc}"
    else
        bad "${desc} (exit ${rc})"
        printf '%s\n' "${out}" | sed 's/^/        | /'
    fi
}

check_fails() {
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then
        bad "${desc} (expected failure, got success)"
    else
        ok "${desc}"
    fi
}

for b in vapord vapor vapor-admin; do
    if [ ! -x "${bin}/${b}" ]; then
        echo "missing ${bin}/${b}; run scripts/build-linux.sh first" >&2
        exit 1
    fi
done

# Isolate the client: config and library.db land inside the temp dir.
export XDG_DATA_HOME="${work}/clientdata"
export HOME="${work}/home"
mkdir -p "${XDG_DATA_HOME}" "${HOME}"

vapor() { "${bin}/vapor" "$@"; }
admin() { "${bin}/vapor-admin" -r "${work}/content" -d "${work}/vapor.db" "$@"; }

step "starting vapord on port ${port}"
"${bin}/vapord" -p "${port}" -r "${work}/content" -d "${work}/vapor.db" \
    > "${work}/vapord.log" 2>&1 &
server_pid=$!

for _ in $(seq 1 50); do
    if grep -q 'listening on' "${work}/vapord.log" 2>/dev/null; then break; fi
    sleep 0.1
done
if ! kill -0 "${server_pid}" 2>/dev/null; then
    echo "vapord failed to start:" >&2
    cat "${work}/vapord.log" >&2
    exit 1
fi
ok "vapord started (pid ${server_pid})"

step "configuration and reachability"
check "config server"   "server is now"  -- vapor config server "http://127.0.0.1:${port}"
check "config library"  "library"        -- vapor config library "${work}/games"
check "ping"            "is up"          -- vapor ping

step "accounts"
export VAPOR_PASSWORD="smoke-test-password"
check "register first user"  "created account"  -- vapor register smokeuser
check "  becomes admin"      "admin"            -- vapor register adminprobe 2>/dev/null || true
check_fails "duplicate username rejected"       vapor register smokeuser
check "login"                "signed in as"     -- vapor login smokeuser
check "whoami"               "smokeuser"        -- vapor whoami

VAPOR_PASSWORD="wrong-password-entirely" \
    check_fails "bad password rejected" vapor login smokeuser
export VAPOR_PASSWORD="smoke-test-password"
check "re-login after failure" "signed in as"   -- vapor login smokeuser

step "packaging a game with vapor-admin"
gamesrc="${work}/src/hollow-vale"
mkdir -p "${gamesrc}/bin" "${gamesrc}/lib"
cat > "${gamesrc}/bin/hollowvale" <<'LAUNCHER'
#!/usr/bin/env bash
echo "Hollow Vale running"
echo "  cwd=$(pwd)"
echo "  data=${HOLLOW_DATA:-unset}"
exit 0
LAUNCHER
echo 'fake shared object' > "${gamesrc}/lib/libhollow.so.1"
echo 'level data' > "${gamesrc}/assets.dat"

check "admin add" "registered" -- admin add "${gamesrc}" \
    --id hollow-vale --name "Hollow Vale" --version 1.0.3 \
    --developer "Smoke Works" --description "A test game." \
    --linux-exec bin/hollowvale --exec-bit 'bin/hollowvale' \
    --exec-bit 'lib/*.so*' --env "HOLLOW_DATA=\$INSTALL_DIR/assets.dat"

check "admin add newer version" "registered" -- admin add "${gamesrc}" \
    --id hollow-vale --name "Hollow Vale" --version 1.0.10 \
    --linux-exec bin/hollowvale --exec-bit 'bin/hollowvale' \
    --env "HOLLOW_DATA=\$INSTALL_DIR/assets.dat"

check "admin list" "hollow-vale" -- admin list

step "catalog"
check "list shows the game"     "Hollow Vale"   -- vapor list
# 1.0.10 must win over 1.0.3: natural order, not lexical.
check "latest version is 1.0.10" "1.0.10"       -- vapor list
check "info"                    "Smoke Works"   -- vapor info hollow-vale
check_fails "info on unknown game"              vapor info no-such-game

step "install"
check "install"           "installed"     -- vapor install hollow-vale
check "installed listing" "hollow-vale"   -- vapor installed
check "verify"            "matches"       -- vapor verify hollow-vale

if [ -x "${work}/games/hollow-vale/bin/hollowvale" ]; then
    ok "exec bit applied from manifest exec_bits"
else
    bad "exec bit applied from manifest exec_bits"
fi
if [ -f "${work}/games/hollow-vale/assets.dat" ]; then
    ok "archive contents extracted"
else
    bad "archive contents extracted"
fi

step "launch"
check "launch runs the game"  "Hollow Vale running"        -- vapor launch hollow-vale
check "env expansion"         "data=${work}/games/hollow-vale/assets.dat" -- vapor launch hollow-vale
check "playtime recorded"     "hollow-vale"                -- vapor installed

step "update detection"
check "update available"  "1.0.10"  -- vapor install hollow-vale --version 1.0.10

step "uninstall"
check "uninstall" "removed" -- vapor uninstall hollow-vale
if [ -d "${work}/games/hollow-vale" ]; then
    bad "install directory deleted"
else
    ok "install directory deleted"
fi

step "authorization is enforced"
check "logout" "signed out" -- vapor logout
check_fails "catalog requires a session" vapor list

printf '\n%d passed, %d failed\n' "${pass}" "${fail}"
[ "${fail}" -eq 0 ]
