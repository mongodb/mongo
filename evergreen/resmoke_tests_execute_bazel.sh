# Executes resmoke suite bazel test targets.
#
# Usage:
#   bash resmoke_tests_execute_bazel.sh
#
# Required environment variables:
# * ${targets} - Resmoke bazel target, like //buildscripts/resmokeconfig:core

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv

source ./evergreen/bazel_utility_functions.sh

BAZEL_BINARY=$(bazel_get_binary_path)

# Timeout is set here to avoid the build hanging indefinitely, still allowing
# for retries.
TIMEOUT_CMD=""
if [ -n "${build_timeout_seconds}" ]; then
  TIMEOUT_CMD="timeout ${build_timeout_seconds}"
fi

if [ ${should_shuffle} = true ]; then
  bazel_args="${bazel_args} --test_arg=--shuffle"
elif [ ${should_shuffle} = false ]; then
  bazel_args="${bazel_args} --test_arg=--shuffleMode=off"
fi

if [ "${is_patch}" = "true" ]; then
  bazel_args="${bazel_args} --test_arg=--patchBuild"
fi

if [ "${skip_symbolization}" = "true" ]; then
  bazel_args="${bazel_args} --test_arg=--skipSymbolization"
fi

# Add test selection flag based on patch parameter
if [ "${enable_evergreen_api_test_selection}" = "true" ]; then
  bazel_args="${bazel_args} --test_arg=--enableEvergreenApiTestSelection"
fi

# Split comma separated list of strategies
IFS=',' read -a strategies <<< "$test_selection_strategies_array"
for strategy in "${strategies[@]}"; do
  bazel_args+=" --test_arg=--evergreenTestSelectionStrategy=${strategy}"
done

set +o errexit

for i in {1..3}; do
  eval ${TIMEOUT_CMD} ${BAZEL_BINARY} fetch ${bazel_args} ${targets} && RET=0 && break || RET=$? && sleep 60
  if [ $RET -eq 124 ]; then
    echo "Bazel fetch timed out after ${build_timeout_seconds} seconds, retrying..."
  else
    echo "Bazel fetch failed, retrying..."
  fi
  $BAZEL_BINARY shutdown
done

eval ${BAZEL_BINARY} test ${bazel_args} ${targets}
RET=$?

set -o errexit

# For a target //path:test, the undeclared test outputs are in
# bazel-testlogs/path/test/test.outputs/outputs.zip. Extract them
# to the root of the task working directory
find bazel-testlogs/$(sed "s|//||;s|:|/|" <<< ${targets}) -name outputs.zip | xargs -I {} unzip -qo {} -d ../

# Combine reports from potentially multiple tests/shards.
find ../ -name report*.json | xargs $python buildscripts/combine_reports.py --no-report-exit -o report.json

exit $RET
