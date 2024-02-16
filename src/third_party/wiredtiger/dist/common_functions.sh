#!/bin/bash

t=__wt.$$
setup_trap() { trap 'rm -f $t' 0 1 2 3 13 15; }

DIST_DIR="$(realpath "$(dirname -- "${BASH_SOURCE[0]}")")"
TOP_DIR="$(dirname "$DIST_DIR")"

cd_top() { cd -- "$TOP_DIR" || exit $?; }
cd_dist() { cd -- "$DIST_DIR" || exit $?; }

check_fast_mode() {
  if [[ "$(realpath $(git rev-parse --show-toplevel))" != "$TOP_DIR" ]]; then
      echo "ERROR: Fast mode only works in WT repo. Please run $0 without -F flag."
      exit 1
  fi
}

is_fast_mode() {
  local arg;
  for arg in "${BASH_ARGV[*]:-}"; do
    [[ "$arg" == "-F" ]] && return;
  done
}

check_fast_mode_flag() { is_fast_mode && check_fast_mode; }

last_commit_from_dev() { python3 "$DIST_DIR"/common_functions.py last_commit_from_dev; }

use_pygrep() {
  [[ "$(uname -s)" == Darwin ]] && EGREP="$DIST_DIR/pygrep.py -E" FGREP="$DIST_DIR/pygrep.py -F" || EGREP=egrep FGREP=fgrep
}
