# Usage:
#   bazel_test [arguments]
#
# Required environment variables:
# * ${targets} - Test targets
# * ${bazel_args} - Extra command line args to pass to "bazel test"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose
set -o pipefail

. "$DIR/bazel_evergreen_shutils.sh"

bazel_evergreen_shutils::activate_and_cd_src
bazel_evergreen_shutils::export_ssl_paths_if_needed

# Use `eval` to force evaluation of the environment variables in the echo statement:
eval echo "Execution environment: Targets: ${targets}"

BAZEL_BINARY="$(bazel_evergreen_shutils::bazel_get_binary_path)"

# Mode-specific LOCAL_ARG and release flag
LOCAL_ARG="$(bazel_evergreen_shutils::compute_local_arg test)"

# If we are doing a patch build or we are building a non-push
# build on the waterfall, then we don't need the --release
# flag. Otherwise, this is potentially a build that "leaves
# the building", so we do want that flag.
RELEASE_FLAG="$(bazel_evergreen_shutils::maybe_release_flag)"

# Possibly scale test timeout and append to bazel_args
bazel_evergreen_shutils::maybe_scale_test_timeout_and_append

# Build the shared flags and persist the --config subset
ALL_FLAGS="--verbose_failures ${LOCAL_ARG} ${bazel_args:-} ${bazel_compile_flags:-} ${task_compile_flags:-} --define=MONGO_VERSION=${version} $RELEASE_FLAG ${patch_compile_flags:-}"
echo "${ALL_FLAGS}" > .bazel_build_flags

# to capture exit codes
set +o errexit

# Build then test with retries.
bazel_evergreen_shutils::retry_bazel_cmd 3 "$BAZEL_BINARY" \
  build ${ALL_FLAGS} ${targets}
RET=$?

if [[ "$RET" == "0" ]]; then

  bazel_evergreen_shutils::retry_bazel_cmd 3 "$BAZEL_BINARY" \
    test ${ALL_FLAGS} ${targets}
  RET=$?

  if [[ "$RET" -eq 124 ]]; then
    echo "Bazel timed out after ${build_timeout_seconds:-<unspecified>} seconds."
  elif [[ "$RET" != "0" ]]; then
    echo "Errors were found during bazel test, failing the execution"
  fi
fi

# For a target //path:test, the undeclared test outputs are in
# bazel-testlogs/path/test/test.outputs/outputs.zip
outputs=bazel-testlogs/$(sed "s|//||;s|:|/|" <<< ${targets})/test.outputs/outputs.zip
if [ -f $outputs ]; then
  unzip $outputs -d ../
fi

set -o errexit

# The --config flag needs to stay consistent between invocations to avoid evicting the previous results.
# Strip out anything that isn't a --config flag that could interfere with the run command.
if [[ "$RET" != "0" ]]; then
  CONFIG_FLAGS="$(bazel_evergreen_shutils::extract_config_flags "${ALL_FLAGS}")"
  eval ${BAZEL_BINARY} run ${CONFIG_FLAGS} //buildscripts:gather_failed_unittests || true
fi

: "${RET:=1}"
exit "${RET}"
