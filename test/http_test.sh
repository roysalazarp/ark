#!/usr/bin/env bash
# HTTP behaviour tests: keep-alive, pipelining, framing, rejections, timeouts.
# Usage: ./test/http_test.sh [--slow]    (--slow also runs the 5s and 10s timeout tests)
set -uo pipefail

cd "$(dirname "$0")/.."

PORT=8181
FAILURES=0

# ARK_TEST_SERVER picks the binary (default ./debug/server). ARK_TEST_WRAPPER runs the server under another tool, e.g. ARK_TEST_WRAPPER="valgrind -q".
${ARK_TEST_WRAPPER:-} "${ARK_TEST_SERVER:-./debug/server}" "$PORT" 2>"${ARK_TEST_LOG:-/dev/null}" &
SERVER_PID=$!
trap 'kill -INT $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null' EXIT

for _ in $(seq 100); do
    nc -z 127.0.0.1 "$PORT" 2>/dev/null && break
    sleep 0.1
done

# send <raw request> [seconds to keep the connection open after sending]
send() {
    (printf "$1"; sleep "${2:-0.3}") | nc -q 1 127.0.0.1 "$PORT" | tr -d '\r'
}

statuses() {
    grep -a '^HTTP/1.1' | cut -d' ' -f2 | tr '\n' ' ' | sed 's/ $//'
}

check() {
    local name="$1" expected="$2" actual="$3"
    if [[ "$actual" == "$expected" ]]; then
        echo "ok   $name"
    else
        echo "FAIL $name: expected [$expected], got [$actual]"
        FAILURES=$((FAILURES + 1))
    fi
}

check "curl reuses the connection" "1" \
    "$(curl -sv "http://127.0.0.1:$PORT/" "http://127.0.0.1:$PORT/nope" 2>&1 | grep -ci 're-using existing connection')"

check "pipelined requests answered in order" "200 404 200" \
    "$(send 'GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /nope HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | statuses)"

check "request split across writes" "200" \
    "$( (printf 'GET / HTTP/1.1\r\nHo'; sleep 0.3; printf 'st: x\r\nConnection: close\r\n\r\n') | nc -q 1 127.0.0.1 "$PORT" | tr -d '\r' | statuses)"

check "body skipped using Content-Length" "405 200" \
    "$(send 'POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhelloGET / HTTP/1.1\r\nConnection: close\r\n\r\n' | statuses)"

check "Transfer-Encoding rejected" "501" \
    "$(send 'POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\nGET / HTTP/1.1\r\n\r\n' | statuses)"

check "duplicate Content-Length rejected" "400" \
    "$(send 'POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nxGET / HTTP/1.1\r\n\r\n' | statuses)"

check "invalid Content-Length rejected" "400" \
    "$(send 'POST / HTTP/1.1\r\nContent-Length: 1x\r\n\r\n' | statuses)"

check "space before header colon rejected" "400" \
    "$(send 'GET / HTTP/1.1\r\nContent-Length : 0\r\n\r\n' | statuses)"

check "bare LF in headers rejected" "400" \
    "$(send 'GET / HTTP/1.1\r\nHost: x\nEvil: y\r\n\r\n' | statuses)"

check "unknown HTTP version rejected" "400" \
    "$(send 'GET / HTTP/2.0\r\n\r\n' | statuses)"

check "oversized headers rejected" "431" \
    "$(send "GET / HTTP/1.1\r\nX: $(head -c 9000 /dev/zero | tr '\0' a)\r\n\r\n" | statuses)"

check "oversized body rejected" "413" \
    "$(send 'POST / HTTP/1.1\r\nContent-Length: 100000\r\n\r\n' | statuses)"

check "HTTP/1.0 closes by default" "close" \
    "$(send 'GET / HTTP/1.0\r\n\r\n' | grep -ai '^connection:' | cut -d' ' -f2)"

check "HTTP/1.0 keep-alive on request" "keep-alive" \
    "$(send 'GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n' | grep -ai '^connection:' | cut -d' ' -f2)"

check "Connection: close honoured" "close" \
    "$(send 'GET / HTTP/1.1\r\nConnection: close\r\n\r\n' | grep -ai '^connection:' | cut -d' ' -f2)"

# Sends a request and reads until the server closes; prints "<statuses> <seconds>".
send_and_time() {
    local start result
    start=$(date +%s)
    exec 3<>"/dev/tcp/127.0.0.1/$PORT"
    printf "$1" >&3
    result=$(timeout 20 cat <&3 | tr -d '\r' | statuses)
    exec 3<&-
    echo "$result $(( $(date +%s) - start ))"
}

# Timeouts are checked about once per tick (1s), so allow one extra second.
check_within() {
    local name="$1" expected_status="$2" low="$3" high="$4" actual="$5"
    local status="${actual% *}" seconds="${actual##* }"
    if [[ "$status" == "$expected_status" ]] && ((seconds >= low && seconds <= high)); then
        echo "ok   $name (${seconds}s)"
    else
        echo "FAIL $name: expected [$expected_status] in ${low}-${high}s, got [$actual]"
        FAILURES=$((FAILURES + 1))
    fi
}

if [[ "${1:-}" == "--slow" ]]; then
    check_within "idle connection closed after 5s" "200" 5 6 \
        "$(send_and_time 'GET / HTTP/1.1\r\n\r\n')"
    check_within "incomplete request gets 408 after 10s" "408" 10 11 \
        "$(send_and_time 'GET / HTTP/1.1\r\nHost: x')"
fi

if ((FAILURES > 0)); then
    echo "$FAILURES test(s) failed"
    exit 1
fi
echo "All tests passed"
