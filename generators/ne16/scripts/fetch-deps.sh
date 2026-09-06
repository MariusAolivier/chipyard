#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
bender=${BENDER:-"$root/.tools/bender"}

if [[ ! -x "$bender" ]]; then
  echo "Bender not found at $bender; run scripts/fetch-bender.sh" >&2
  exit 1
fi

cd "$root"
for attempt in 1 2 3 4 5 6 7 8; do
  if "$bender" --git-throttle 1 checkout; then
    exit 0
  fi
  sleep $((attempt * 15))
done

echo "Unable to fetch locked dependencies after 8 attempts" >&2
exit 1
