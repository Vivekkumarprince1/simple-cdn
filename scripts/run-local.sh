#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"

if [ ! -x ./simple-cdn ]; then
  ./scripts/build.sh
fi

pids=""
cleanup() {
  echo "Stopping local CDN..."
  for pid in $pids; do
    kill "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM HUP

start_edge() {
  region=$1
  port=$2
  CDN_ROUTER_TOKEN=local-development-token ./simple-cdn public "$port" 86400 "$region" &
  pids="$pids $!"
}

start_edge north 8081
start_edge west-central 8082
start_edge south-east 8083
./simple-cdn router config/cdn.local.toml &
pids="$pids $!"

attempt=0
until curl -fsS http://127.0.0.1:8080/ready >/dev/null 2>&1; do
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 40 ]; then
    echo "Local CDN failed to start. Check the startup messages above." >&2
    exit 1
  fi
  sleep 0.1
done

echo ""
echo "Simple CDN is running at http://localhost:8080"
echo "Press Ctrl+C to stop all four services."
wait
