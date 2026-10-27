#!/usr/bin/env bash
# Run one benchmark point on suzhou950: start press_server pinned to a cpuset,
# run press_client pinned to another cpuset, stop the server, keep the logs.
#
# Usage:
#   run_point.sh --server-cpus LIST --client-cpus LIST \
#                --server-args "..." --client-args "..." --out DIR \
#                [--perf EVENTS --perf-delay SECS --perf-secs SECS]
#
# Writes DIR/{server.log,client.log,threads.txt[,perf.txt]} and prints the
# client's RESULT line and the server's QUEUE_STATS line (if any) to stdout.
set -euo pipefail

CA=${CA:-$HOME/cluster_aware}
BIN=${BIN:-$CA/build/bench}
PORT=${PORT:-8002}

server_cpus="" client_cpus="" server_args="" client_args="" out=""
perf_events="" perf_delay=3 perf_secs=10
while [ $# -gt 0 ]; do
  case $1 in
    --server-cpus) server_cpus=$2; shift 2 ;;
    --client-cpus) client_cpus=$2; shift 2 ;;
    --server-args) server_args=$2; shift 2 ;;
    --client-args) client_args=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --perf) perf_events=$2; shift 2 ;;
    --perf-delay) perf_delay=$2; shift 2 ;;
    --perf-secs) perf_secs=$2; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
[ -n "$server_cpus" ] && [ -n "$client_cpus" ] && [ -n "$out" ] \
  || { echo "missing --server-cpus/--client-cpus/--out" >&2; exit 2; }

listening() { ss -ltn "sport = :$PORT" | grep -q LISTEN; }

# Many IO threads (e.g. 192 EventBases) need more fds than the default 1024.
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

mkdir -p "$out"
if listening; then
  echo "port $PORT already in use" >&2
  exit 1
fi

# shellcheck disable=SC2086
taskset -c "$server_cpus" "$BIN/press_server" --port="$PORT" $server_args \
  > "$out/server.log" 2>&1 &
spid=$!
trap 'kill -9 $spid 2>/dev/null || true' EXIT

for _ in $(seq 300); do
  listening && break
  kill -0 "$spid" 2>/dev/null || { echo "server exited early" >&2; tail -5 "$out/server.log" >&2; exit 1; }
  sleep 0.1
done
listening || { echo "server not listening after 30s" >&2; exit 1; }

ps -L -o tid=,comm= -p "$spid" | while read -r tid comm; do
  echo "$comm $(taskset -cp "$tid" | awk '{print $NF}')"
done > "$out/threads.txt"

perf_pid=""
if [ -n "$perf_events" ]; then
  (sleep "$perf_delay"
   sudo -n perf stat -x, -e "$perf_events" -p "$spid" -- sleep "$perf_secs" \
     2> "$out/perf.txt" > /dev/null) &
  perf_pid=$!
fi

crc=0
# shellcheck disable=SC2086
taskset -c "$client_cpus" "$BIN/press_client" --port="$PORT" $client_args \
  > "$out/client.log" 2>&1 || crc=$?
[ -n "$perf_pid" ] && wait "$perf_pid" || true

kill -TERM "$spid" 2>/dev/null || true
for _ in $(seq 100); do
  kill -0 "$spid" 2>/dev/null || break
  sleep 0.1
done
kill -9 "$spid" 2>/dev/null || true
wait "$spid" 2>/dev/null || true
trap - EXIT

[ "$crc" -eq 0 ] || { echo "client exited $crc" >&2; tail -5 "$out/client.log" >&2; exit 1; }
grep -a '^RESULT ' "$out/client.log" || { echo "no RESULT line" >&2; exit 1; }
grep -a '^QUEUE_STATS ' "$out/server.log" || true
