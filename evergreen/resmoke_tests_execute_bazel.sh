# Executes resmoke suite bazel test targets.
#
# Usage:
#   bash resmoke_tests_execute_bazel.sh
#
# Required environment variables:
# * ${targets} - Resmoke bazel target, like //buildscripts/resmokeconfig:core

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
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

ci_flags="--//bazel/resmoke:in_evergreen --test_output=all --noincompatible_enable_cc_toolchain_resolution --repo_env=no_c++_toolchain=1"

if [ ${should_shuffle} = true ]; then
    ci_flags+=" --test_arg=--shuffle"
elif [ ${should_shuffle} = false ]; then
    ci_flags+=" --test_arg=--shuffleMode=off"
fi

if [ "${is_patch}" = "true" ]; then
    ci_flags+=" --test_arg=--patchBuild"
fi

if [ "${skip_symbolization}" = "true" ]; then
    ci_flags+=" --test_arg=--skipSymbolization"
fi

# Add test selection flag based on patch parameter
if [ "${enable_evergreen_api_test_selection}" = "true" ]; then
    ci_flags+=" --test_arg=--enableEvergreenApiTestSelection"
fi

# Split comma separated list of strategies
IFS=',' read -a strategies <<<"$test_selection_strategies_array"
for strategy in "${strategies[@]}"; do
    ci_flags+=" --test_arg=--evergreenTestSelectionStrategy=${strategy}"
done

# If not explicitly specified on the target, pick a shard count that will fully utilize the current machine.
BUILD_INFO=$(bazel query ${targets} --output build)
if [[ "$BUILD_INFO" != *"shard_count ="* ]]; then
    CPUS=$(nproc)
    SIZE=$(echo $BUILD_INFO | grep "size =" | cut -d '"' -f2)
    TEST_RESOURCES_CPU=$(echo ${bazel_args} ${bazel_compile_flags} ${task_compile_flags} ${patch_compile_flags} | awk -F'--default_test_resources=cpu=' '{print $2}')
    TEST_RESOURCES_CPU=(${TEST_RESOURCES_CPU//,/ })
    declare -A SIZES
    SIZES=(["small"]=0 ["medium"]=1 ["large"]=2 ["enormous"]=3)
    CPUS_PER_SHARD=${TEST_RESOURCES_CPU[${SIZES[$SIZE]}]}
    SHARD_COUNT=$((CPUS / $CPUS_PER_SHARD))
    ci_flags+=" --test_sharding_strategy=forced=$SHARD_COUNT"
fi

set +o errexit

for i in {1..3}; do
    eval ${TIMEOUT_CMD} ${BAZEL_BINARY} fetch ${ci_flags} ${bazel_args} ${bazel_compile_flags} ${task_compile_flags} ${patch_compile_flags} ${targets} && RET=0 && break || RET=$? && sleep 60
    if [ $RET -eq 124 ]; then
        echo "Bazel fetch timed out after ${build_timeout_seconds} seconds, retrying..."
    else
        echo "Bazel fetch failed, retrying..."
    fi
    $BAZEL_BINARY shutdown
done

# Save the invocation, intentionally excluding ci_flags.
echo "python buildscripts/install_bazel.py" >bazel-invocation.txt
echo "bazel test ${bazel_args} ${bazel_compile_flags} ${task_compile_flags} ${patch_compile_flags} ${targets}" >>bazel-invocation.txt

eval ${BAZEL_BINARY} test ${ci_flags} ${bazel_args} ${bazel_compile_flags} ${task_compile_flags} ${patch_compile_flags} ${targets}
RET=$?

set -o errexit

# Symlink data directories to where Resmoke normally puts them for compatability with post tasks
# that run for all Resmoke tasks.
find bazel-testlogs/ -path '*data/job*' -name 'job*' -print0 |
    while IFS= read -r -d '' test_outputs; do
        source=${workdir}/src/$test_outputs
        target=${workdir}/$(sed 's/.*\.outputs\///' <<<$test_outputs)
        mkdir -p $(dirname $target)
        ln -sf $source $target
    done

# Symlink test logs to where Evergreen expects them. Evergreen won't read into a symlinked directory,
# so symlink each log file individually.
find bazel-testlogs/ -type f -path "*TestLogs/*" -print0 |
    while IFS= read -r -d '' test_outputs; do
        source=${workdir}/src/$test_outputs
        target=${workdir}/$(sed 's/.*\.outputs\///' <<<$test_outputs)
        mkdir -p $(dirname $target)
        ln -sf $source $target
    done

# Combine reports from potentially multiple tests/shards.
find bazel-testlogs/ -name report*.json | xargs $python buildscripts/combine_reports.py --no-report-exit -o report.json

exit $RET
