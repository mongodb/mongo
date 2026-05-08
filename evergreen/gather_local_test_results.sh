# Gathers locally-executed bazel test results from bazel-testlogs/ and prepares them
# for Evergreen ingestion in the same layout as fetch_remote_test_results.sh.
#
# Usage:
#   bash gather_local_test_results.sh
#
# Required environment variables:
# * ${test_label} - The bazel test target, e.g. //buildscripts/resmokeconfig:core
# * ${workdir}    - The Evergreen workdir.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/bazel_test_results_shutils.sh"

readonly target_prefix=$(bazel_test_results::label_to_prefix "${test_label}")

readonly bazel_testlogs="${workdir}/src/bazel-testlogs"
readonly target_outputs="${bazel_testlogs}/${target_prefix}"

if [ ! -d "${target_outputs}" ]; then
    echo "Error: No bazel test outputs found at ${target_outputs}" >&2
    echo "The test may have failed to build. Check the logs from the runner task." >&2
    exit 1
fi

# Collect shard directories. Sharded tests use shard_<N>_of_<M>/; single-shard tests put files
# directly under the target's directory and are treated as shard 0.
declare -a shard_paths=()
declare -a shard_nums=()
if compgen -G "${target_outputs}/shard_*_of_*" >/dev/null; then
    while IFS= read -r shard_dir; do
        shard_num=$(basename "${shard_dir}" | sed 's/shard_\([0-9]\+\)_of_.*/\1/')
        shard_paths+=("${shard_dir}")
        shard_nums+=("${shard_num}")
    done < <(find "${target_outputs}" -maxdepth 1 -type d -name 'shard_*_of_*' | sort)
else
    shard_paths+=("${target_outputs}")
    shard_nums+=("0")
fi

shard_names=()
shard_statuses=()
shard_test_counts=()
fail_task=0

for i in "${!shard_paths[@]}"; do
    shard_dir="${shard_paths[$i]}"
    shard_num="${shard_nums[$i]}"
    shard_path="${target_prefix}/shard_${shard_num}"
    target_dir="${workdir}/results/${shard_path}"
    mkdir -p "${target_dir}"

    is_failure_flag=0
    is_timeout_flag=0

    # Copy test.log so it ends up in results/<prefix>/shard_<N>/shard_<N>_test.log,
    # matching the naming convention used by fetch_remote_test_results.sh and picked up
    # by the teardown S3 put filter "**/*test.log".
    if [ -f "${shard_dir}/test.log" ]; then
        cp "${shard_dir}/test.log" "${target_dir}/shard_${shard_num}_test.log"
    fi

    # Locate the undeclared outputs zip produced by --zip_undeclared_test_outputs.
    output_zip=""
    for candidate in \
        "${shard_dir}/test.outputs/outputs.zip" \
        "${shard_dir}/outputs.zip"; do
        if [ -f "${candidate}" ]; then
            output_zip="${candidate}"
            break
        fi
    done

    if [ -n "${output_zip}" ]; then
        mkdir -p "${target_dir}/test.outputs"
        unzip -o -q "${output_zip}" -d "${target_dir}/test.outputs"
    fi

    pushd "${target_dir}" >/dev/null
    bazel_test_results::symlink_test_logs

    # Determine pass/fail from the extracted report.json. record_shard_status appends to the
    # parallel summary arrays and returns non-zero if no report was found.
    if compgen -G "test.outputs/report*.json" >/dev/null; then
        report_file=$(compgen -G "test.outputs/report*.json" | head -n 1)
        failed_tests=$(jq '.results | map(select(.status == "fail" or .status == "timeout")) | length' "${report_file}" 2>/dev/null || echo 0)
        if [[ "${failed_tests}" -gt 0 ]]; then
            is_failure_flag=1
            fail_task=1
        fi
    fi
    bazel_test_results::record_shard_status \
        "$shard_path" "$is_failure_flag" "$is_timeout_flag" \
        shard_names shard_statuses shard_test_counts || true

    # If this shard failed and produced an outputs zip, keep a copy alongside results/ so
    # the teardown S3 put filter "**/*outputs.zip" attaches it to the task.
    if [[ "${is_failure_flag}" -eq 1 && -n "${output_zip}" ]]; then
        cp "${output_zip}" "shard_${shard_num}_test.outputs.zip"
    fi
    popd >/dev/null
done

# Surface bazel's saved invocation (written by save_invocation in resmoke_tests_execute_bazel.sh)
# at ${workdir}/bazel-invocation.txt for the teardown S3 put.
if [ -f "${workdir}/src/bazel-invocation.txt" ]; then
    cp "${workdir}/src/bazel-invocation.txt" "${workdir}/bazel-invocation.txt"
fi

bazel_test_results::print_executor_logs

bazel_test_results::display_test_summary shard_names shard_statuses shard_test_counts

bazel_test_results::combine_metrics

bazel_test_results::combine_reports

# Check for system-level failures (TIMEOUT or NO_REPORT)
for status in "${shard_statuses[@]}"; do
    if [[ "${status}" == "TIMEOUT" || "${status}" == "NO_REPORT" ]]; then
        echo "Error: One or more shards had TIMEOUT or NO_REPORT status. Not all tests ran or were reported." >&2
        bazel_test_results::write_test_failures_expansion
        exit 1
    fi
done

if [[ "${fail_task}" -eq 1 ]]; then
    bazel_test_results::write_test_failures_expansion
fi

exit 0
