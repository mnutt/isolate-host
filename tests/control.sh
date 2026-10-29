#!/usr/bin/env bash

set -euo pipefail

host=$1
client=$2
socket=$3
log=${socket}.log

rm -f "$socket" "$log"
"$host" "$socket" >"$log" 2>&1 &
host_pid=$!

cleanup() {
  status=$?
  kill "$host_pid" 2>/dev/null || true
  wait "$host_pid" 2>/dev/null || true
  rm -f "$socket"
  if [[ "$status" -ne 0 ]]; then
    cat "$log" >&2
  else
    rm -f "$log"
  fi
  exit "$status"
}
trap cleanup EXIT

for _attempt in $(seq 1 100); do
  if [[ -S "$socket" ]]; then
    break
  fi
  sleep 0.05
done

[[ -S "$socket" ]]
set -- /proc/"$host_pid"/task/*
baseline_threads=$#
timeout 60 "$client" "$socket"
set -- /proc/"$host_pid"/task/*
final_threads=$#
[[ "$final_threads" -le $((baseline_threads + 2)) ]]
grep -q '"message":"isolate-host-log-marker","worker":"workers:testgrain123"' "$log"
grep -q '"message":"isolate-host-log-marker","worker":"workers:cpugrain123"' "$log"

kill "$host_pid"
wait "$host_pid" 2>/dev/null || true
rm -f "$socket"

ISOLATE_HOST_IDLE_TIMEOUT_MS=200 "$host" "$socket" >>"$log" 2>&1 &
host_pid=$!
for _attempt in $(seq 1 100); do
  if [[ -S "$socket" ]]; then
    break
  fi
  sleep 0.05
done

[[ -S "$socket" ]]
timeout 60 "$client" "$socket" --idle-eviction
