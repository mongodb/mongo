unset workdir
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o nounset
set -o pipefail
set -o verbose

cd src

ALL_TESTS_FILE="buildscripts/convert_scons_to_bazel/converted_unittests.txt"
EXCLUDED_TESTS_FILE="buildscripts/convert_scons_to_bazel/windows_excluded_tests.txt"

total_tests=$(grep -cEv '^\s*(#|$)' "$ALL_TESTS_FILE")
excluded_tests=$(grep -cEv '^\s*(#|$)' "$EXCLUDED_TESTS_FILE")
expected_tests=$((total_tests - excluded_tests))

mapfile -d '' test_binaries < <(find bazel-bin/install-windows_unittests -name "*.exe" -print0)
found_tests=${#test_binaries[@]}

if [ "$found_tests" -ne "$expected_tests" ]; then
  echo "ERROR: Found $found_tests test binaries, expected $expected_tests."
  exit 1
fi

FAILED_TESTS_FILE="$(mktemp)"

run_test() {
  local test_exe="$1"
  local logfile="${test_exe}.log"

  "$test_exe" > "$logfile" 2>&1
  local status=$?
  if [ $status -ne 0 ]; then
    echo "Test $test_exe failed with exit code $status. Output:"
    cat "$logfile"
    echo "$test_exe" >> "$FAILED_TESTS_FILE"
  fi
  return 0 # Always return 0 so xargs runs all tests
}

export FAILED_TESTS_FILE
export -f run_test

set +e

printf "%s\0" "${test_binaries[@]}" \
  | xargs -0 -n 1 -P "$(nproc)" -I{} bash -c 'run_test "{}"'

set -e

if [[ -s "$FAILED_TESTS_FILE" ]]; then
  echo "ERROR: One or more tests failed:"
  cat "$FAILED_TESTS_FILE"
  rm "$FAILED_TESTS_FILE"
  exit 1
else
  rm "$FAILED_TESTS_FILE"
  echo "All tests passed."
fi
