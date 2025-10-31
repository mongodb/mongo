#!/usr/bin/env bash
set -euo pipefail

# Repo root from script location
cd "$(dirname "$0")"/..

# Exec real gdb with all original args
exec bazel run gdb -- "$@"
