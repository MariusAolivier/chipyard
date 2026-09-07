#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
bender=${BENDER:-"$root/.tools/bender"}

cd "$root/upstream"
for attempt in 1 2 3 4 5 6 7 8; do
  if "$bender" checkout; then
    exit 0
  fi
  sleep $((attempt * 15))
done

echo "Unable to fetch locked ITA dependencies after 8 attempts" >&2
exit 1
