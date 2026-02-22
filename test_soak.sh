#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
PORT=18554
PATH_NAME="/live/stream"
DURATION_SEC=120
TRANSPORT="udp"
AUTH_CRED=""
USE_DIGEST=0
REPORT_FILE=""

usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  --duration <sec>      Soak duration in seconds (default: ${DURATION_SEC})
  --port <port>         RTSP port (default: ${PORT})
  --path <path>         RTSP path (default: ${PATH_NAME})
  --transport <udp|tcp> Client preferred transport (default: ${TRANSPORT})
  --auth <user:pass>    Enable server auth and use credentials on client URL
  --digest              Use Digest auth (default: Basic if --auth is set)
  --report <file>       Write markdown report to specific file
  -h, --help            Show help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration)
            DURATION_SEC="$2"
            shift 2
            ;;
        --port)
            PORT="$2"
            shift 2
            ;;
        --path)
            PATH_NAME="$2"
            shift 2
            ;;
        --transport)
            TRANSPORT="$2"
            shift 2
            ;;
        --auth)
            AUTH_CRED="$2"
            shift 2
            ;;
        --digest)
            USE_DIGEST=1
            shift
            ;;
        --report)
            REPORT_FILE="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown arg: $1"
            usage
            exit 1
            ;;
    esac
done

if [[ "${TRANSPORT}" != "udp" && "${TRANSPORT}" != "tcp" ]]; then
    echo "Invalid --transport: ${TRANSPORT}"
    exit 1
fi
if [[ -n "${AUTH_CRED}" && "${AUTH_CRED}" != *:* ]]; then
    echo "Invalid --auth format, expected user:pass"
    exit 1
fi

if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "Build dir missing: ${BUILD_DIR}"
    echo "Run: mkdir -p build && cd build && cmake .. && cmake --build . -j"
    exit 1
fi

SERVER_BIN=""
CLIENT_BIN=""
if [[ -x "${BUILD_DIR}/examples/example_server" ]]; then
    SERVER_BIN="${BUILD_DIR}/examples/example_server"
    CLIENT_BIN="${BUILD_DIR}/examples/example_client"
elif [[ -x "${BUILD_DIR}/example_server" ]]; then
    SERVER_BIN="${BUILD_DIR}/example_server"
    CLIENT_BIN="${BUILD_DIR}/example_client"
else
    echo "Cannot find example_server/example_client in ${BUILD_DIR}"
    exit 1
fi

TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="${SCRIPT_DIR}/soak_reports/${TS}"
mkdir -p "${OUT_DIR}"
SERVER_LOG="${OUT_DIR}/server.log"
CLIENT_LOG="${OUT_DIR}/client.log"
if [[ -z "${REPORT_FILE}" ]]; then
    REPORT_FILE="${OUT_DIR}/report.md"
fi

SERVER_PID=""
CLIENT_RC=0
SERVER_RC=0

cleanup() {
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -INT "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

wait_for_port() {
    local host="$1"
    local port="$2"
    local timeout_sec="$3"
    local i=0
    while (( i < timeout_sec * 10 )); do
        if (echo >"/dev/tcp/${host}/${port}") >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

echo "[soak] output dir: ${OUT_DIR}"
echo "[soak] starting server..."
SERVER_ARGS=("${PORT}" "${PATH_NAME}" "--log-format" "json" "--log-level" "info")
if [[ -n "${AUTH_CRED}" ]]; then
    SERVER_ARGS+=("--auth" "${AUTH_CRED}")
    if [[ "${USE_DIGEST}" -eq 1 ]]; then
        SERVER_ARGS+=("--digest")
    fi
fi
"${SERVER_BIN}" "${SERVER_ARGS[@]}" >"${SERVER_LOG}" 2>&1 &
SERVER_PID=$!

if ! wait_for_port "127.0.0.1" "${PORT}" 10; then
    echo "[soak] server start timeout"
    exit 1
fi

CLIENT_URL="rtsp://127.0.0.1:${PORT}${PATH_NAME}"
if [[ -n "${AUTH_CRED}" ]]; then
    CLIENT_URL="rtsp://${AUTH_CRED}@127.0.0.1:${PORT}${PATH_NAME}"
fi
echo "[soak] client url: ${CLIENT_URL}"
CLIENT_ARGS=("${CLIENT_URL}" "--duration" "${DURATION_SEC}" "--log-format" "json" "--log-level" "info")
if [[ "${TRANSPORT}" == "tcp" ]]; then
    CLIENT_ARGS+=("--prefer-tcp")
fi

echo "[soak] running client for ${DURATION_SEC}s..."
set +e
"${CLIENT_BIN}" "${CLIENT_ARGS[@]}" >"${CLIENT_LOG}" 2>&1
CLIENT_RC=$?
set -e

echo "[soak] stopping server..."
if kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill -INT "${SERVER_PID}" 2>/dev/null || true
fi
set +e
wait "${SERVER_PID}"
SERVER_RC=$?
set -e
SERVER_PID=""

SERVER_STATS_LINE="$(grep 'SERVER_STATS ' "${SERVER_LOG}" | tail -n 1 || true)"
CLIENT_STATS_LINE="$(grep 'CLIENT_STATS ' "${CLIENT_LOG}" | tail -n 1 || true)"
ERROR_LINES_SERVER="$(grep -Ec '\\[ERROR\\]|\"level\":\"ERROR\"' "${SERVER_LOG}" || true)"
ERROR_LINES_CLIENT="$(grep -Ec '\\[ERROR\\]|\"level\":\"ERROR\"' "${CLIENT_LOG}" || true)"

RESULT="PASS"
if [[ ${CLIENT_RC} -ne 0 || ${SERVER_RC} -ne 0 ]]; then
    RESULT="FAIL"
fi
if [[ -z "${SERVER_STATS_LINE}" || -z "${CLIENT_STATS_LINE}" ]]; then
    RESULT="FAIL"
fi

cat > "${REPORT_FILE}" <<EOF
# RTSP Soak Test Report

- Timestamp (UTC): ${TS}
- Result: ${RESULT}
- Duration: ${DURATION_SEC}s
- Transport: ${TRANSPORT}
- Auth: $( [[ -n "${AUTH_CRED}" ]] && echo "$( [[ ${USE_DIGEST} -eq 1 ]] && echo digest || echo basic )" || echo "disabled" )
- Server RC: ${SERVER_RC}
- Client RC: ${CLIENT_RC}
- Server log: ${SERVER_LOG}
- Client log: ${CLIENT_LOG}

## Error Counts

- server error lines: ${ERROR_LINES_SERVER}
- client error lines: ${ERROR_LINES_CLIENT}

## Parsed Stats

\`\`\`
${SERVER_STATS_LINE}
${CLIENT_STATS_LINE}
\`\`\`
EOF

echo "[soak] report: ${REPORT_FILE}"
echo "[soak] result: ${RESULT}"

if [[ "${RESULT}" != "PASS" ]]; then
    exit 1
fi
