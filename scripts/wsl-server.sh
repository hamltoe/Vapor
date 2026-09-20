#!/usr/bin/env bash
# Run vapord inside WSL with data on the Linux filesystem (SQLite is unhappy
# on /mnt/c). The Windows client reaches it at http://127.0.0.1:8777.
#
#   scripts/wsl-server.sh start|stop|restart|status
#   powershell -File scripts/start-wsl-server.ps1 [start|stop|status]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ -n "${VAPOR_ROOT:-}" ]; then
    root="${VAPOR_ROOT}"
fi
bin="${root}/build-linux/bin/vapord"
data="${VAPOR_SERVER_DIR:-${HOME}/vapor}"
library="${VAPOR_LIBRARY_DIR:-${data}/library}"
port="${VAPOR_PORT:-8777}"
pidfile="${data}/vapord.pid"
logfile="${data}/vapord.log"

usage() {
    sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'
}

running_pid() {
    if [ -f "${pidfile}" ]; then
        local pid
        pid="$(cat "${pidfile}")"
        if [ -n "${pid}" ] && kill -0 "${pid}" 2>/dev/null; then
            echo "${pid}"
            return 0
        fi
    fi
    return 1
}

cmd_status() {
    local pid
    if pid="$(running_pid)"; then
        echo "vapord is running (pid ${pid})"
        echo "  bind ........ 0.0.0.0:${port}"
        echo "  library ..... ${library}"
        echo "  content ..... ${data}/content"
        echo "  database .... ${data}/vapor.db"
        echo "  log ......... ${logfile}"
        if command -v curl >/dev/null; then
            echo -n "  health ...... "
            curl -fsS --max-time 2 "http://127.0.0.1:${port}/api/v1/health" || echo "(not answering)"
            echo
        fi
        return 0
    fi
    echo "vapord is not running"
    return 1
}

cmd_stop() {
    local pid
    if ! pid="$(running_pid)"; then
        rm -f "${pidfile}"
        echo "vapord is not running"
        return 0
    fi
    kill "${pid}" 2>/dev/null || true
    for _ in $(seq 1 25); do
        kill -0 "${pid}" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "${pid}" 2>/dev/null; then
        kill -9 "${pid}" 2>/dev/null || true
    fi
    rm -f "${pidfile}"
    echo "stopped vapord (pid ${pid})"
}

cmd_start() {
    if [ ! -x "${bin}" ]; then
        echo "missing ${bin}" >&2
        echo "build it in WSL with: bash scripts/bootstrap-linux.sh --server-only && bash scripts/build-linux.sh" >&2
        exit 1
    fi
    mkdir -p "${data}/content" "${library}"

    local pid
    if pid="$(running_pid)"; then
        echo "vapord already running (pid ${pid})"
        cmd_status || true
        return 0
    fi

    if command -v ss >/dev/null && ss -ltn | grep -q ":${port} "; then
        echo "port ${port} is already in use by another process." >&2
        echo "stop that vapord first, or set VAPOR_PORT to a free port." >&2
        ss -ltnp 2>/dev/null | grep ":${port} " || true
        exit 1
    fi

    nohup "${bin}" -H 0.0.0.0 -p "${port}" \
        -r "${data}/content" \
        -L "${library}" \
        -d "${data}/vapor.db" \
        >>"${logfile}" 2>&1 &
    echo $! >"${pidfile}"
    disown || true

    for _ in $(seq 1 30); do
        if curl -fsS --max-time 1 "http://127.0.0.1:${port}/api/v1/health" >/dev/null 2>&1; then
            echo "vapord ${port} is up"
            echo "  Windows client: vapor config server http://127.0.0.1:${port}"
            echo "  data .......... ${data}"
            echo "  library ....... ${library}"
            return 0
        fi
        sleep 0.1
    done
    echo "vapord did not become healthy; last log lines:" >&2
    tail -n 20 "${logfile}" >&2 || true
    exit 1
}

cmd="${1:-status}"
case "${cmd}" in
start)   cmd_start ;;
stop)    cmd_stop ;;
restart) cmd_stop; cmd_start ;;
status)  cmd_status ;;
-h|--help|help) usage ;;
*)
    echo "wsl-server: unknown command ${cmd}" >&2
    usage >&2
    exit 1
    ;;
esac
