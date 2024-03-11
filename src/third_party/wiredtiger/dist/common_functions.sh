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
setup_trap() { trap 'kill `allchildpids` 2>/dev/null; [[ -n "$t" ]] && rm -f $t*; '"$@" 0 1 2 3 13 15; }

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
  FAST_FLAG=""   # Indicates if the fast mode was requested by flag.
  [[ -n "${WT_FAST:-}" ]] && return 0;
  local arg;
  for arg in "${BASH_ARGV[@]:-}"; do
    [[ "$arg" == "-F" ]] && FAST_FLAG="-F" && export WT_FAST="-F" && return 0;
  done
  return 1
}

# Check if the script has file arguments.
# Assume files are any argument that doesn't begin with a "-".
has_file_args() {
  for arg in "${BASH_ARGV[@]:-}"; do
    [[ "${arg:0:1}" != "-" ]] && return 0;
  done
  return 1
}

# Check if the fast mode is requested and is available.
# Set variables appropriately or exit with an error.
check_fast_mode_flag() {
  is_fast_mode || return
  check_fast_mode
  if [[ -n "${FAST_FLAG:-}" ]] && has_file_args; then
    echo "ERROR: Fast mode (-F flag) cannot be used with file arguments"
    exit 1
  fi
}

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

# Try running xargs with the -P option. If it succeeds, set XARGS_P accordingly.
# POSIX xargs does not support -P option - it's a GNU extension.
# -P option limits the maximum number of subprocesses created by xargs.
# Without -P, xargs will sun only one subprocess at a time.
# Usage:
#   check_xargs_P [NUM_PROCESSES]
#   xargs $XARGS_P ...
check_xargs_P() {
  [[ -z "${XARGS_P:+x}" ]] &&
    XARGS_P=$(echo echo | xargs -P 1 echo >/dev/null 2>&1 && echo "-P ${1:-20}")
}

# Check if the script is run in a recursive mode (with files args).
# File arguments are those that don't start with a "-".
is_recursive_mode() {
  export -n S_RECURSE      # Unexport S_RECURSE to stop propagation to child processes.
  [[ -n "${S_RECURSE:-}" ]] && return 0         # S_RECURSE is set.
  [[ ${BASH_ARGC:-} -eq 0 ]] && return 1        # No args at all
  for arg in "${BASH_ARGV[@]:-}"; do
    [[ "${arg:0:1}" == "-" ]] && return 1;      # Option arg found.
  done
  return 0
}

# Returns true if the script has to find files to process (non-recursive call).
# The top level script generates a list of files to process and calls itself in parallel mode.
is_main_run() {
  ! is_recursive_mode       # Return the inverted result of is_recursive_mode.
}

# Run the same the script in parallel mode.
# The list of files is provided via stdin.
# filter_if_fast is automatically applied to the list.
# This function has protection against sub-recursive calls from child processes.
#
# Example usage:
#    is_main_run && {
#        find bench examples ext src test -name '*.[ch]' | do_in_parallel
#        exit $?
#    }
#    ... code for processing files when run as parallel or with file arguments ...
#
#  - `is_main_run` checks if the script is run in non-recursive mode (has no file args).
#  - `find ...` lists files to process.
#  - `do_in_parallel` restarts the same script in parallel mode.
#  - `exit $?` exits the script with the return code of `do_in_parallel`.
#
# NOTE: `do_in_parallel` can't do `exit` itself since Bash is allowed to execute piped
# commands in subshells. Calling `exit` from a subshell will not terminate the main program.
# The caller should use `exit` after `do_in_parallel` to terminate the script.
#
# NOTE: Fast mode works only for scripts starting in the TOP directory because
# filter_if_fast uses empty prefix.
#
do_in_parallel() {
  [[ -n "${S_RECURSE:-}" ]] && { echo "ERROR: Possible infinite recursion detected."; exit 1; }
  export S_RECURSE=1
  local name=$(basename $0)
  check_xargs_P
  filter_if_fast | xargs $XARGS_P -n $(( ${#WT_FAST} ? 5 : 30 )) "$@" -- bash -c "
      bash $DIST_DIR/$name \"\$@\" 2>&1 > $t-$name-par-\$\$.out ; RET=\$?;
      cat $t-$name-par-\$\$.out 2>/dev/null
      rm -f $t-$name-par-\$\$.out
      exit \$RET
  " -bash
  # implicit: return $?
}
