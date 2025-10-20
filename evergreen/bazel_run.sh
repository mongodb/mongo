# Usage:
#   bazel_compile [arguments]
#
# Required environment variables:
# * ${target} - Build target
# * ${args} - Extra command line args to pass to "bazel run"
# * ${env} - Env variable string to set (ex. ENV_VAR_ABC=123)
# * ${redact_args} - If set, redact the args in the report

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose
set -o pipefail

. "$DIR/bazel_evergreen_shutils.sh"

bazel_evergreen_shutils::activate_and_cd_src
bazel_evergreen_shutils::export_ssl_paths_if_needed

# Use `eval` to force evaluation of the environment variables in the echo statement:
eval echo "Execution environment: Args: ${args} Target: ${target} Env: ${env} redact_args: ${redact_args}"

BAZEL_BINARY="$(bazel_evergreen_shutils::bazel_get_binary_path)"

# Build LOCAL_ARG for run-mode
LOCAL_ARG="$(bazel_evergreen_shutils::compute_local_arg run)"
# Honor .bazel_build_flags --config lines (do not evict previous cache)
ALL_FLAGS=""
if [[ -f .bazel_build_flags ]]; then
  ALL_FLAGS="$(< .bazel_build_flags)"
fi
CONFIG_FLAGS="$(bazel_evergreen_shutils::extract_config_flags "${ALL_FLAGS}")"
LOCAL_ARG="${CONFIG_FLAGS} ${LOCAL_ARG}"

INVOCATION_WITH_REDACTION="${target}"
if [[ -z "${redact_args:-}" ]]; then
  INVOCATION_WITH_REDACTION+=" ${args}"
fi

# Record invocation
echo "bazel run --verbose_failures ${LOCAL_ARG} ${INVOCATION_WITH_REDACTION}" > bazel-invocation.txt

# capture exit code
set +o errexit

bazel_evergreen_shutils::retry_bazel_cmd 5 "$BAZEL_BINARY" \
  run --verbose_failures ${LOCAL_ARG} ${target} ${args} 2>&1 | tee -a bazel_output.log
RET=${PIPESTATUS[0]}
: "${RET:=1}"
set -o errexit

# Report
$python ./buildscripts/simple_report.py \
  --test-name "bazel run ${INVOCATION_WITH_REDACTION}" \
  --log-file bazel_output.log \
  --exit-code "${RET}"

exit "${RET}"
