#!/bin/bash

#
# Common functions for the WiredTiger dist/ scripts.
#
# Usage:
# For convenience, this script will execute its arguments as commands.
# A recommended way to use it is to put this in the first line of your script:
#
#    . `dirname -- ${BASH_SOURCE[0]}`/common_functions.sh
#    setup_trap
#    cd_top
#    check_fast_mode_flag
#
# What it does is:
# - Initializes the `$t` variable for temporary files.
# - Sets up a trap to clean up temporary files and kill child processes on exit or abort.
# - Changes the current directory to the top directory of the WiredTiger tree.
#   - (Use `cd_dist` to change the directory to the `dist/` directory).
# - Checks if the fast mode is requested and is available.
#   - (Use it if the fast mode is supported by your script).
#

t=__wt.$$

# Recursively get PIDs of all child processes.
allchildpids() {
    ps -o pid,ppid | python3 -c '
import re, os, sys
children = {}   # Map from parent PID to list of child PIDs.
for line in sys.stdin.readlines():
    (pid, ppid) = [*re.findall(r"(\d+)", line), 0, 0][:2]
    if pid and ppid and pid != str(os.getpid()):
        if ppid not in children: children[ppid] = []
        children[ppid].append(pid)
pids = children[sys.argv[1]].copy() if sys.argv[1] in children else []
while len(pids):
    (pid, pids) = (pids[0], pids[1:])   # pop one element and shift the list.
    print(pid)
    if pid in children: pids.extend(children[pid])
' $$
}

# Do normal cleanup on exit or abort.
setup_trap() { trap 'kill `allchildpids` 2>/dev/null; rm -f $t' 0 1 2 3 13 15; }

# Absolute path to the `dist/` directory.
DIST_DIR="$(realpath "$(dirname -- "${BASH_SOURCE[0]}")")"
# Absolute path to the top directory of WiredTiger tree.
TOP_DIR="$(dirname "$DIST_DIR")"

# Chdir to the top directory of WiredTiger tree.
cd_top() { cd -- "$TOP_DIR" || exit $?; }
# Chdir to the `dist/` directory.
cd_dist() { cd -- "$DIST_DIR" || exit $?; }

# Check if the fast mode is available at all.
check_fast_mode() {
  if [[ "$(realpath $(git rev-parse --show-toplevel))" != "$TOP_DIR" ]]; then
    echo "ERROR: Fast mode only works in WT repo. Please run $0 without -F flag."
    exit 1
  fi
}

# Check if the fast mode is requested.
is_fast_mode() {
  [[ -n "${WT_FAST:-}" ]] && return 0;
  local arg;
  for arg in "${BASH_ARGV[@]:-}"; do
    [[ "$arg" == "-F" ]] && export WT_FAST="-F" && return 0;
  done
  return 1
}

# Check if the fast mode is requested and is available.
check_fast_mode_flag() { is_fast_mode && check_fast_mode; }

# Get the earliest common commit from the dev branch.
last_commit_from_dev() { python3 "$DIST_DIR"/common_functions.py last_commit_from_dev; }

# Initialise the pygrep for MacOS.
use_pygrep() {
  [[ "$(uname -s)" == Darwin ]] && EGREP="$DIST_DIR/pygrep.py -E" FGREP="$DIST_DIR/pygrep.py -F" || EGREP=egrep FGREP=fgrep
}

# Get a list of changed files from the last commit from the dev branch.
changed_files() { git diff --name-only `last_commit_from_dev` "$@"; }
# Convert a list of lines to a regex.
lines_to_regex() { sed -E -e 's/([]/.()*+[])/\\\1/' -e 's/^(.*)$/(\1)/' | paste -s -d '|' -; }
# Filter a list of files with ones that were changed.
filter_changed_files() {
  [[ -z "${EGREP:-}" ]] && use_pygrep
  local PREFIX="${1:-^}"
  local REGEX="$(changed_files | lines_to_regex)"
  local REGEX="$PREFIX($REGEX)"
  $EGREP $REGEX
}

# If fast mode is requested,
# then filter a list of files with ones that were changed;
# else pass the list through.
filter_if_fast() {
  if is_fast_mode; then
    filter_changed_files "$@"
  else
    cat
  fi
}
