#!/usr/bin/env bash
# Benchmarks Ark against an equivalent Node.js server with wrk.
#
# Usage: ./bench/run.sh
# Env:   DURATION (default 10s), CONNECTIONS (default 100)
#        NODES: space-separated Node.js binaries to compare (default: node)
#        e.g. NODES="node /opt/node-v26.13.0/bin/node" ./bench/run.sh
#
# The server is pinned to CPU 0 and wrk to CPU 1, so they don't compete for
# a core. Results compare servers on this machine; they are not absolute
# capacity numbers.
set -euo pipefail

cd "$(dirname "$0")/.."

DURATION="${DURATION:-10s}"
CONNECTIONS="${CONNECTIONS:-100}"
PORT=8282
read -r -a NODES <<<"${NODES:-node}"

for tool in wrk taskset nc "${NODES[@]}"; do
    command -v "$tool" >/dev/null || { echo "ERROR: $tool not installed"; exit 1; }
done

if (($(nproc) < 2)); then
    echo "WARNING: fewer than 2 CPUs; server and wrk will share a core"
fi

./build.sh >/dev/null
./build.sh release >/dev/null

# bench <name> <extra request header, or ""> -- <server command...>
bench() {
    local name="$1" header="$2"
    shift 3

    taskset -c 0 "$@" "$PORT" 2>/dev/null &
    local pid=$!

    for _ in $(seq 100); do
        nc -z 127.0.0.1 "$PORT" 2>/dev/null && break
        sleep 0.1
    done

    local out
    local wrk_args=(-t1 -c"$CONNECTIONS" -d"$DURATION" --latency)
    # wrk silently ignores headers unless written "Name: value", with the space.
    [[ -n "$header" ]] && wrk_args+=(-H "$header")
    out=$(taskset -c 1 wrk "${wrk_args[@]}" "http://127.0.0.1:$PORT/")

    local peak_kb
    peak_kb=$(awk '/VmHWM/ {print $2}' "/proc/$pid/status")

    kill -INT "$pid"
    wait "$pid" 2>/dev/null || true

    local rps p50 p99 errors
    rps=$(awk '/Requests\/sec/ {printf "%.0f", $2}' <<<"$out")
    p50=$(awk '$1 == "50%" {print $2}' <<<"$out")
    p99=$(awk '$1 == "99%" {print $2}' <<<"$out")
    errors=$(grep -E 'Socket errors|Non-2xx' <<<"$out" | tr -s ' ' | tr '\n' ';' || true)

    printf "%-34s %10s %9s %9s %8s MB  %s\n" "$name" "$rps" "$p50" "$p99" \
        "$(awk -v kb="$peak_kb" 'BEGIN {printf "%.1f", kb / 1024}')" "${errors:-}"
}

echo "wrk: 1 thread, $CONNECTIONS connections, $DURATION per run"
echo
printf "%-34s %10s %9s %9s %11s\n" "server" "req/s" "p50" "p99" "peak mem"

bench "Ark debug (-O0)" "" -- ./debug/server
bench "Ark release (-O2)" "" -- ./release/server
for node in "${NODES[@]}"; do
    bench "Node.js $("$node" --version)" "" -- "$node" bench/node_server.js
done

bench "Ark release, no keep-alive" "Connection: close" -- ./release/server
for node in "${NODES[@]}"; do
    bench "Node.js $("$node" --version), no keep-alive" "Connection: close" -- "$node" bench/node_server.js
done
