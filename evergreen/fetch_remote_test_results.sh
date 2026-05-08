# Downloads bazel test results from remote executions and prepares them for Evergreen ingestion.
#
# Usage:
#   bash fetch_remote_test_results.sh
#
# Assumes the following files exist:
#  ./src/"build_events.json" (or set path in $BEP_FILE)      Build events JSON containing the records of remote test executions
#  engflow.cert and engflow.key located in either ${workdir}/src or ${HOME}/.engflow/creds
#
# Required environment variables:
# * ${test_label} - The resmoke bazel target to get results for, like //buildscripts/resmokeconfig:core
# * ${workdir} - The Evergreen workdir to use for test log and OTel trace ingestion.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/bazel_test_results_shutils.sh"

# Enumerates test results for each execution of ${test_label}. Shards/retries are individual executions with their own results.
function enumerate_test_results() {
    jq --raw-output --compact-output --arg test_label "${test_label}" 'select(.testResult.testActionOutput != null) |
        .id.testResult as $id |
        select($id.label == $test_label)' "$BEP_FILE"
}

# Checks if a test result record indicates that the test failed.
function is_failure() {
    jq --exit-status '.testResult | select(.status != "PASSED")' <<<$1 >/dev/null
}

# Checks if a test result record indicates that the test timed out.
function is_timeout() {
    jq --exit-status '.testResult | select(.status == "TIMEOUT")' <<<$1 >/dev/null
}

# Returns a file-path safe prefix for an individual test execution.
function target_prefix() {
    jq --raw-output '.id.testResult as $id | .testResult | "\(($id.label | ltrimstr("//") | gsub(":";"\/")))/shard_\($id.shard)"' <<<$1
}

# Downloads the test outputs from EngFlow for a given test result record.
function download_outputs() {
    local test_result=$1
    local is_failure=$2

    jq --raw-output '.id.testResult as $id | .testResult.testActionOutput[] | "\t\($id.shard)\t\(.name)\t\(.uri)"' <<<"$test_result" | while IFS=$'\t' read -r shard name uri; do
        # Always download test.outputs (zip file) and test.log
        # If test failed, also download manifest
        should_download=false
        if [[ "$name" == *'test.outputs'* && "$name" != *'manifest'* ]]; then
            should_download=true
        fi
        if [[ "$name" == *'test.log'* ]]; then
            should_download=true
        fi
        if [[ "$is_failure" == "1" ]]; then
            if [[ "$name" == *'manifest__MANIFEST'* ]]; then
                should_download=true
            fi
        fi

        name="shard_${shard}_${name}"

        if [[ "$should_download" == 'true' ]]; then
            # Convert URIs like bytestream://sodalite.cluster.engflow.com/default/blobs/... to
            # https://sodalite.cluster.engflow.com/api/contentaddressablestorage/v1/instances/default/blobs/...
            if [[ "$uri" =~ ^bytestream://([^/]+)/blobs/(.+)$ ]]; then
                host=$(echo "$uri" | sed 's|^bytestream://\([^/]*\)/blobs/.*|\1|')
                blob_path=$(echo "$uri" | sed 's|^bytestream://[^/]*/blobs/\(.*\)|\1|')
                https_uri="https://$host/api/contentaddressablestorage/v1/instances/default/blobs/$blob_path"
            else
                https_uri="$uri"
            fi
            echo "  Downloading: $name"
            curl --silent --show-error --cert "$ENGFLOW_CERT" --key "$ENGFLOW_KEY" --output "$name" "$https_uri"
        fi
    done
}

# Unzips a test's undeclared outputs zip, and removes the zip if the test passed.
function unzip_outputs() {
    local is_failure=$1

    # Find test.outputs zip file (excluding manifest files)
    local zip_file=$(find . -maxdepth 1 -name '*test.outputs*' -name '*.zip' ! -name '*manifest*' -type f | head -n 1)

    if [[ -n "$zip_file" ]]; then
        local output_dir='test.outputs'
        mkdir -p "$output_dir"
        unzip -o -q "$zip_file" -d "$output_dir"

        # If test passed, remove the zip file. If it failed, the whole thing is going to be attached to the task in Evergreen.
        if [[ "$is_failure" != '1' ]]; then
            rm "$zip_file"
        fi
    fi
}

# Writes a user-friendly bazel invocation for re-running this test target.
function write_bazel_invocation() {
    # Escape special characters in the label for the second sed expression.
    local test_label_escaped=$(echo "$test_label" | sed 's/[][\/\.\*^$]/\\&/g')
    mkdir -p "${workdir}/src/"
    sed "s/\S*\$/${test_label_escaped}/" ${workdir}/resmoke-tests-bazel-invocation.txt | tail -n 1 >"${workdir}/bazel-invocation.txt"
}

# Resolves a file path from a list of candidate locations. Returns the first existing file path found.
function resolve_file() {
    local -n paths=$1
    for path in "${paths[@]}"; do
        if [ -f "$path" ]; then
            echo "$path"
            return 0
        fi
    done
    return 1
}

BEP_FILE="${BEP_FILE:-src/build_events.json}"

if ! [ -f "$ENGFLOW_CERT" ]; then
    cert_candidates=(
        "${workdir}/src/engflow.cert"
        "${HOME}/.engflow/creds/engflow.crt"
    )
    ENGFLOW_CERT=$(resolve_file cert_candidates)
fi

if ! [ -f "$ENGFLOW_KEY" ]; then
    key_candidates=(
        "${workdir}/src/engflow.key"
        "${HOME}/.engflow/creds/engflow.key"
    )
    ENGFLOW_KEY=$(resolve_file key_candidates)
fi

if [ ! -f "$BEP_FILE" ]; then
    echo "Error: File '$BEP_FILE' not found" >&2
    exit 1
fi

if [ ! -f "$ENGFLOW_CERT" ]; then
    echo "Error: EngFlow certificate not found at '$ENGFLOW_CERT'" >&2
    exit 1
fi

if [ ! -f "$ENGFLOW_KEY" ]; then
    echo "Error: EngFlow key not found at '$ENGFLOW_KEY'" >&2
    exit 1
fi

fail_task=0
result_count=0
missing_report=0
shard_names=()
shard_statuses=()
shard_test_counts=()

echo "Fetching test results for ${test_label}..."

while IFS= read -r test_result; do
    ((result_count++))
    target_prefix=$(target_prefix "$test_result")
    target_dir="${workdir}/results/$target_prefix"
    mkdir -p "$target_dir"
    pushd "$target_dir" >/dev/null

    is_failure_flag=0
    is_timeout_flag=0
    if is_timeout "$test_result"; then
        is_timeout_flag=1
        is_failure_flag=1
        fail_task=1
        bazel_test_results::write_test_failures_expansion
    elif is_failure "$test_result"; then
        is_failure_flag=1
        fail_task=1
        bazel_test_results::write_test_failures_expansion
    fi

    download_outputs "$test_result" "$is_failure_flag"
    unzip_outputs "$is_failure_flag"
    bazel_test_results::symlink_test_logs

    if ! bazel_test_results::record_shard_status \
        "$target_prefix" "$is_failure_flag" "$is_timeout_flag" \
        shard_names shard_statuses shard_test_counts; then
        if [[ "$is_timeout_flag" -ne 1 ]]; then
            missing_report=1
        fi
    fi

    popd >/dev/null
done < <(enumerate_test_results)

# Check if any results were found
if [[ "$result_count" -eq 0 ]]; then
    echo "Error: No test results found for target '${test_label}' in '$BEP_FILE'." >&2
    echo "The test may have failed to build. Check the logs from the resmoke_tests task." >&2
    exit 1
fi

bazel_test_results::print_executor_logs

bazel_test_results::display_test_summary shard_names shard_statuses shard_test_counts

bazel_test_results::combine_metrics

failures=$(bazel_test_results::combine_reports)

write_bazel_invocation

# Check for system-level failures (TIMEOUT or NO_REPORT)
for status in "${shard_statuses[@]}"; do
    if [[ "$status" == "TIMEOUT" || "$status" == "NO_REPORT" ]]; then
        echo "Error: One or more shards had TIMEOUT or NO_REPORT status. Not all tests ran or were reported." >&2
        bazel_test_results::write_test_failures_expansion
        exit 1
    fi
done

# Check for test failures
# If there are test failures, write the expansion and exit 0 to let Evergreen mark as failed via test results
has_test_failures=0
for status in "${shard_statuses[@]}"; do
    if [[ "$status" == "FAILED" ]]; then
        has_test_failures=1
        break
    fi
done

if [[ "$has_test_failures" -eq 1 ]]; then
    bazel_test_results::write_test_failures_expansion
fi

exit 0
