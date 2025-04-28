# Usage:
#   bazel_test [arguments]
#
# Required environment variables:
# * ${targets} - Test targets
# * ${bazel_args} - Extra command line args to pass to "bazel test"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv

set -o pipefail

# Use `eval` to force evaluation of the environment variables in the echo statement:
eval echo "Execution environment: Targets: ${targets}"

source ./evergreen/bazel_utility_functions.sh
source ./evergreen/bazel_RBE_supported.sh

LOCAL_ARG=""
if [[ "${evergreen_remote_exec}" != "on" ]]; then
  LOCAL_ARG="$LOCAL_ARG --jobs=auto"
fi

BAZEL_BINARY=$(bazel_get_binary_path)

# Timeout is set here to avoid the build hanging indefinitely, still allowing
# for retries.
TIMEOUT_CMD=""
if [ -n "${build_timeout_seconds}" ]; then
  TIMEOUT_CMD="timeout ${build_timeout_seconds}"
fi

if is_ppc64le; then
  LOCAL_ARG="$LOCAL_ARG --jobs=48"
fi

if is_s390x; then
  LOCAL_ARG="$LOCAL_ARG --jobs=16"
fi

# If we are doing a patch build or we are building a non-push
# build on the waterfall, then we don't need the --release
# flag. Otherwise, this is potentially a build that "leaves
# the building", so we do want that flag.
if [ "${is_patch}" = "true" ] || [ -z "${push_bucket}" ] || [ "${compiling_for_test}" = "true" ]; then
  echo "This is a non-release build."
else
  LOCAL_ARG="$LOCAL_ARG --config=public-release"
fi

if [ -n "${test_timeout_sec}" ]; then
  bazel_args="${bazel_args} --test_timeout=${test_timeout_sec}"
fi

ALL_FLAGS="--verbose_failures ${LOCAL_ARG} ${bazel_args} ${bazel_compile_flags} ${task_compile_flags} --define=MONGO_VERSION=${version} ${patch_compile_flags}"
echo ${ALL_FLAGS} > .bazel_build_flags

set +o errexit

for i in {1..3}; do
  eval ${TIMEOUT_CMD} ${BAZEL_BINARY} test ${ALL_FLAGS} ${targets} 2>&1 | tee bazel_stdout.log \
    && RET=0 && break || RET=$? && sleep 1
  if [ $RET -eq 124 ]; then
    echo "Bazel timed out after ${build_timeout_seconds} seconds, retrying..."
  else
    echo "Errors were found during the bazel test, failing the execution"
    break
  fi
done

# For a target //path:test, the undeclared test outputs are in
# bazel-testlogs/path/test/test.outputs/outputs.zip
outputs=bazel-testlogs/$(sed "s|//||;s|:|/|" <<< ${target})/test.outputs/outputs.zip
if [ -f $outputs ]; then
  unzip $outputs -d ../
fi

set -o errexit

if [[ $RET != 0 ]]; then
  # The --config flag needs to stay consistent between invocations to avoid evicting the previous results.
  # Strip out anything that isn't a --config flag that could interfere with the run command.
  CONFIG_FLAGS=$(echo "${ALL_FLAGS}" | tr ' ' '\n' | grep -- '--config' | tr '\n' ' ')

  eval ${BAZEL_BINARY} run ${CONFIG_FLAGS} //buildscripts:gather_failed_unittests
fi

exit $RET
