#!/bin/bash
#
# Minimizer script for manual test case minimization.
#
# Usage:
#
#  This script can be used to minimize a dumped reproducer. For instance, if you
#  ran the fuzz test from the codelab like this:
#
#    FUZZTEST_REPRODUCERS_OUT_DIR=/tmp/reproducers \
#    bazel run --config=fuzztest :escaping_test \
#      -- --fuzz=UnescapingAStringNeverTriggersUndefinedBehavior
#
#  Then when a crash is found, the crashing input is saved to a file and you
#  will see something like this on the terminal:
#
#    [*] Reproducer file written to: /tmp/reproducers/r3pr0dUc3r
#
#  In order to minimize this reproducer input file, run:
#
#    FUZZTEST_MINIMIZE_REPRODUCER=/tmp/reproducers/r3pr0dUc3r \
#    bazel run --config=fuzztest :escaping_test \
#      --run_under=@com_google_fuzztest//tools:minimizer \
#      -- --fuzz=UnescapingAStringNeverTriggersUndefinedBehavior
#
#  This will try to find smaller and smaller inputs until you manually stop the
#  process with Ctrl-C. When you do, you'll get a message like:
#
#    Find the smallest reproducer at:
#
#    /tmp/reproducers/r3pr0dUc3r-min-0008

set -u

readonly ORIGINAL_REPRODUCER="${FUZZTEST_MINIMIZE_REPRODUCER}"

for i in {0001..9999}; do
  echo
  echo "╔════════════════════════════════════════════════╗"
  echo "║ Minimization round: ${i}                       ║"
  echo "╚════════════════════════════════════════════════╝"
  echo

  TEMP_DIR=$(mktemp -d)

  FUZZTEST_REPRODUCERS_OUT_DIR="${TEMP_DIR}" \
  FUZZTEST_MINIMIZE_REPRODUCER="${FUZZTEST_MINIMIZE_REPRODUCER}" \
  "$@" \
  --nosymbolize_stacktrace

  if [ $? -eq 130 ]; then
    echo
    echo "╔═══════════════════════════════════════════════╗"
    echo "║ Minimization terminated.                      ║"
    echo "╚═══════════════════════════════════════════════╝"
    echo
    echo "Find the smallest reproducer at:"
    echo
    echo "${FUZZTEST_MINIMIZE_REPRODUCER}"

    rm -rf "${TEMP_DIR}"
    break
  fi

  SMALLER_REPRODUCER=$(find "${TEMP_DIR}" -type f)
  NEW_NAME="${ORIGINAL_REPRODUCER}-min-${i}"
  mv "${SMALLER_REPRODUCER}" "${NEW_NAME}"
  FUZZTEST_MINIMIZE_REPRODUCER="${NEW_NAME}"

  rm -rf "${TEMP_DIR}"
done
