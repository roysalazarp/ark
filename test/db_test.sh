#!/usr/bin/env bash
# Checks that a slow database query does not block the server.
# Needs the database from compose.yml. The query in the app takes 3 seconds.
# Usage: ./test/db_test.sh
set -uo pipefail

cd "$(dirname "$0")/.."

PORT=8182
URL="http://127.0.0.1:$PORT"
FAILURES=0
LOG="${ARK_TEST_LOG:-$(mktemp)}"

${ARK_TEST_WRAPPER:-} "${ARK_TEST_SERVER:-./debug/server}" "$PORT" 2>"$LOG" &
SERVER_PID=$!
trap 'kill -INT $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null' EXIT

for _ in $(seq 100); do
    nc -z 127.0.0.1 "$PORT" 2>/dev/null && break
    sleep 0.1
done

check() {
    local name="$1" ok="$2" detail="$3"
    if [[ "$ok" == "1" ]]; then
        echo "ok   $name ($detail)"
    else
        echo "FAIL $name ($detail)"
        FAILURES=$((FAILURES + 1))
    fi
}

now_ms() { date +%s%3N; }
below() { awk -v a="$1" -v b="$2" 'BEGIN {print (a < b) ? 1 : 0}'; }
between() { awk -v a="$1" -v lo="$2" -v hi="$3" 'BEGIN {print (a >= lo && a <= hi) ? 1 : 0}'; }

start=$(now_ms)
body=$(curl -s "$URL/slow")
took=$(( $(now_ms) - start ))
check "slow query returns its result" "$([[ "$body" == "hello from postgres at "* ]] && echo 1 || echo 0)" "$body"
check "slow query takes about 3s" "$(between "$took" 2900 3600)" "${took} ms"

# The main test: other clients are served while a query runs.
curl -s -o /dev/null "$URL/slow" &
SLOW_PID=$!
sleep 0.3
worst=0
count=0
while kill -0 "$SLOW_PID" 2>/dev/null; do
    seconds=$(curl -s -o /dev/null -w '%{time_total}' "$URL/")
    ms=$(awk -v s="$seconds" 'BEGIN {printf "%d", s * 1000}')
    ((ms > worst)) && worst=$ms
    count=$((count + 1))
done
check "other requests are not blocked" "$(below "$worst" 100)" "$count requests during the query, slowest ${worst} ms"

start=$(now_ms)
for _ in 1 2 3 4 5; do
    curl -s -o /dev/null "$URL/slow" &
    PIDS+=($!)
done
wait "${PIDS[@]}"
took=$(( $(now_ms) - start ))
check "5 queries run at the same time" "$(between "$took" 2900 4000)" "${took} ms in total, not 15 s"

result=$( (printf 'GET /slow HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\nConnection: close\r\n\r\n'; sleep 4) | nc -q 1 127.0.0.1 "$PORT" | tr -d '\r' | grep -aE '^(hello|Hello)' | cut -d' ' -f1 | tr '\n' ' ')
check "pipelined request waits its turn" "$([[ "$result" == "hello Hello " ]] && echo 1 || echo 0)" "$result"

(printf 'GET /slow HTTP/1.1\r\n\r\n'; sleep 0.5) | nc -q 0 127.0.0.1 "$PORT" >/dev/null
sleep 3.2
check "client leaving mid-query is harmless" "$([[ "$(curl -s "$URL/")" == "Hello from Ark" ]] && echo 1 || echo 0)" "server still answers"

# Postgres may use 20 connections at once, so 10 of these 30 wait for a free one.
start=$(now_ms)
codes=$(for _ in $(seq 30); do curl -s -o /dev/null -w '%{http_code}\n' "$URL/slow" & done | sort | uniq -c | awk '{printf "%s x%s  ", $2, $1}')
took=$(( $(now_ms) - start ))
check "extra queries wait for a connection" "$([[ "$codes" == "200 x30  " ]] && echo 1 || echo 0)" "$codes"
check "waiting queries run next" "$(between "$took" 5900 7500)" "${took} ms in total: two rounds of 3 s"

# The queue holds 64 queries, so 6 of these 70 are turned away.
codes=$(for _ in $(seq 70); do curl -s -o /dev/null -w '%{http_code}\n' "$URL/slow" & done | sort | uniq -c | awk '{printf "%s x%s  ", $2, $1}')
check "a full queue gets 503" "$([[ "$codes" == "200 x64  503 x6  " ]] && echo 1 || echo 0)" "$codes"

# raw_get <path>: sends the path byte for byte and prints the response body.
raw_get() {
    (printf 'GET %s HTTP/1.1\r\nConnection: close\r\n\r\n' "$1"; sleep 0.3) | nc -q 1 127.0.0.1 "$PORT" | tr -d '\r' | sed '1,/^$/d'
}

body=$(raw_get /hello/roy)
check "query parameter is used" "$([[ "$body" == "hello, roy" ]] && echo 1 || echo 0)" "$body"

attack="x';DROP/**/TABLE/**/ark_injection_test;--"
psql -qw -h 127.0.0.1 -p 6432 -U ark -d ark -c 'CREATE TABLE IF NOT EXISTS ark_injection_test (id int)'
body=$(raw_get "/hello/$attack")
exists=$(psql -Atw -h 127.0.0.1 -p 6432 -U ark -d ark -c "SELECT to_regclass('ark_injection_test') IS NOT NULL")
psql -qw -h 127.0.0.1 -p 6432 -U ark -d ark -c 'DROP TABLE IF EXISTS ark_injection_test'
check "SQL in a parameter is only text" "$([[ "$body" == "hello, $attack" ]] && echo 1 || echo 0)" "$body"
check "SQL in a parameter does not run" "$([[ "$exists" == "t" ]] && echo 1 || echo 0)" "table still exists: $exists"

code=$(curl -s -o /dev/null -w '%{http_code}' "$URL/hello/$(head -c 5000 /dev/zero | tr '\0' a)")
check "parameter too large gets 414" "$([[ "$code" == "414" ]] && echo 1 || echo 0)" "$code"

code=$( (printf 'GET /hello/\xff HTTP/1.1\r\nConnection: close\r\n\r\n'; sleep 0.3) | nc -q 1 127.0.0.1 "$PORT" | head -1 | cut -d' ' -f2)
check "invalid text is rejected by the database" "$([[ "$code" == "502" ]] && echo 1 || echo 0)" "$code"

body=$(curl -s "$URL/rows/3")
check "all rows and columns come back" "$([[ "$body" == $'1\t10\t\n2\t20\tNULL\n3\t30\t' ]] && echo 1 || echo 0)" "$(echo "$body" | tr '\t\n' ',;')"
check "NULL and empty text are different" "$([[ "$(echo "$body" | cut -f3 | tr '\n' ',')" == ",NULL,," ]] && echo 1 || echo 0)" "third column: $(echo "$body" | cut -f3 | tr '\n' ',')"
check "the count is reported" "$(grep -q '3 rows, 3 columns, count 3$' "$LOG" && echo 1 || echo 0)" "$(grep -o '3 rows, 3 columns, count [0-9]*$' "$LOG" | tail -1)"

lines=$(curl -s "$URL/rows/2000" | wc -l)
check "results bigger than 8 KB" "$([[ "$lines" == "2000" ]] && echo 1 || echo 0)" "$lines rows"

code=$(curl -s -o /dev/null -w '%{http_code}' "$URL/rows/200000")
check "results over 1 MB fail" "$([[ "$code" == "502" ]] && grep -q 'result too large' "$LOG" && echo 1 || echo 0)" "$code, $(grep -o 'result too large' "$LOG" | tail -1)"

code=$(curl -s -o /dev/null -w '%{http_code}' "$URL/rows/abc")
check "the error code is kept" "$([[ "$code" == "502" ]] && grep -q '\[22P02\]' "$LOG" && echo 1 || echo 0)" "$code, $(grep -o '\[22P02\][^"]*' "$LOG" | tail -1)"

echo "--- server log (last 3 lines)"
tail -3 "$LOG"

if ((FAILURES > 0)); then
    echo "$FAILURES test(s) failed"
    exit 1
fi
echo "All tests passed"
