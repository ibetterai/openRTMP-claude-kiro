#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="/Users/huilinzhu/Projects/OpenRTMP"
cd "$REPO_ROOT"
exec python3 "$REPO_ROOT/tools/kiro-parallel/kiro_parallel.py" "$@"
