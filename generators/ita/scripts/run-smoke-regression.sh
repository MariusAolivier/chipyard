#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
timeout "${TIMEOUT:-30m}" "$root/build-verilator/obj-smoke/ita-hwpe-smoke"
