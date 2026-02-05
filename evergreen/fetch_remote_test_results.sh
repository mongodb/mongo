# Downloads bazel test results from remote executions and prepares them for Evergreen ingestion.
#
# Usage:
#   bash fetch_remote_test_results.sh
#
# Assumes the following files exist:
#  ./"build_events.json"                       Build events JSON containing the records of remote test executions
#  engflow.cert and engflow.key located in either ${workdir}/src or ${HOME}/.engflow/creds
#
# Required environment variables:
# * ${test_label} - The resmoke bazel target to get results for, like //buildscripts/resmokeconfig:core
# * ${workdir} - The Evergreen workdir to use for test log and OTel trace ingestion.

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

# Returns a file-path safe prefix for an individual test execution.
function target_prefix() {
    jq --raw-output '.id.testResult as $id | .testResult | "\(($id.label | ltrimstr("//") | gsub(":";"\/")))/shard_\($id.shard)"' <<<$1
}

# Downloads the test outputs from EngFlow for a given test result record.
function download_outputs() {
    local test_result=$1
    local is_failure=$2

    jq --raw-output '.id.testResult as $id | .testResult.testActionOutput[] | "\t\($id.shard)\t\(.name)\t\(.uri)"' <<<"$test_result" | while IFS=$'\t' read -r shard name uri; do
        # Always download test.outputs (zip file)
        # If test failed, also download test.log and manifest
        should_download=false
        if [[ "$name" == *'test.outputs'* && "$name" != *'manifest'* ]]; then
            should_download=true
        fi
        if [[ "$is_failure" == "1" ]]; then
            if [[ "$name" == *'test.log'* || "$name" == *'manifest__MANIFEST'* ]]; then
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
        echo "  Unzipping: $zip_file -> $output_dir/"
        unzip -o -q "$zip_file" -d "$output_dir"

        # If test passed, remove the zip file. If it failed, the whole thing is going to be attached to the task in Evergreen.
        if [[ "$is_failure" != '1' ]]; then
            echo "  Removing: $zip_file"
            rm "$zip_file"
        fi
    fi
}

# Symlinks test logs from a test result into Evergreen's log ingestion folder.
function symlink_test_logs() {
    local build_dir='test.outputs/build/TestLogs'

    if [[ ! -d "$build_dir" ]]; then
        return
    fi

    echo "  Creating symlinks in ${workdir}/build/..."
    find "$build_dir" -type f | while read -r file; do
        # Get the relative path from the build directory
        rel_path="${file#$build_dir/}"
        target_path="${workdir}/build/TestLogs/${rel_path}"
        target_dir=$(dirname "$target_path")

        mkdir -p "$target_dir"

        abs_file=$(realpath "$file")
        ln -sf "$abs_file" "$target_path"
    done
}

# Combine all resmoke telemetry and place it where Evergreen expects it: ${workdir}/build/OTelTraces.
# Metrics are batched into line-separated JSON files no greater than 4MB each. Evergreen processes
# fewer files faster, but hits message size limitations if they are too large.
function combine_metrics() {
    local output_dir="${workdir}/build/OTelTraces"
    mkdir -p "$output_dir"

    local max_size=$((4 * 1024 * 1024)) # 4MB in bytes
    local file_counter=0
    local current_size=0
    local current_output="${output_dir}/metrics.json"

    # Create initial empty file
    >"$current_output"

    find "${workdir}/results" -wholename '*metrics/metrics*.json' -type f -print0 | while IFS= read -r -d '' file; do
        local file_size=$(stat -c%s "$file")
        local newline_size=1

        # Check if adding this file would exceed the limit
        if ((current_size + file_size + newline_size > max_size && current_size > 0)); then
            # Start a new file
            ((file_counter++))
            current_output="${output_dir}/metrics_${file_counter}.json"
            current_size=0
            >"$current_output"
        fi

        # Append the file content
        cat "$file" >>"$current_output"
        echo "" >>"$current_output" # Adds a single newline after each file's content

        # Update current size
        current_size=$((current_size + file_size + newline_size))
    done

    echo 'Combined OTel metrics json'
}

# Combines all Resmoke test report JSONs into a single JSON.
function combine_reports() {
    local report_files=$(find "${workdir}" -name 'report*.json' -type f 2>/dev/null)

    if [[ -z "$report_files" ]]; then
        echo 'No report.json files found'
        return
    fi

    local combined_report=$(echo "$report_files" | xargs jq -s '
        {
            results: map(.results // []) | add,
            failures: (map(.results // []) | add | map(select(.status == "fail" or .status == "timeout")) | length)
        }
    ')

    local combined_report_file="${workdir}/report.json"
    echo "$combined_report" >"$combined_report_file"
    echo "Combined report written to: $combined_report_file"

    local total_tests=$(echo "$combined_report" | jq '.results | length')
    local failures=$(echo "$combined_report" | jq '.failures')
    echo "Summary: $total_tests tests, $failures failures"
}

# Writes a user-friendly bazel invocation for re-running this test target.
function write_bazel_invocation() {
    # Escape special characters in the label for the second sed expression.
    local test_label_escaped=$(echo "$test_label" | sed 's/[][\/\.\*^$]/\\&/g')
    mkdir -p "${workdir}/src/"
    sed "s/\S*\$/${test_label_escaped}/" ${workdir}/resmoke-tests-bazel-invocation.txt | tail -n 1 >"${workdir}/bazel-invocation.txt"
}

# Writes a YAML file indicating that test failures exist.
function write_test_failures_expansion() {
    local output_file="${workdir}/results/test_failures_exist.yml"
    mkdir -p "$(dirname "$output_file")"
    echo "test_failures_exist: true" >"$output_file"
}

# Print the contents of all *test.log files.
function print_executor_logs() {
    echo "Executor logs for all failed shards:"
    find "${workdir}/results" -name '*test.log' -type f -exec cat {} +
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

BEP_FILE='build_events.json'

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
while IFS= read -r test_result; do
    target_prefix=$(target_prefix "$test_result")
    target_dir="${workdir}/results/$target_prefix"
    echo "Fetching results for $target_prefix"
    mkdir -p "$target_dir"
    pushd "$target_dir" >/dev/null

    is_failure_flag=0
    if is_failure "$test_result"; then
        is_failure_flag=1
        fail_task=1
        write_test_failures_expansion
    fi

    download_outputs "$test_result" "$is_failure_flag"
    unzip_outputs "$is_failure_flag"
    symlink_test_logs

    popd >/dev/null
done < <(enumerate_test_results)

combine_metrics

failures=$(combine_reports)

write_bazel_invocation

# If there are failures, let Evergreen mark the test as failed by the test results by exiting 0 here.
# If there are no failures in the combined report, but the bazel test failed, report
# it as a system failure by returning $fail_task.
if [[ "$failures" == 'No report.json files found' ]]; then
    if [[ "$fail_task" -eq 1 ]]; then
        echo 'No report/test logs were found, but the bazel test failed. Check the test executor logs below.'
    fi
    write_test_failures_expansion
    print_executor_logs
    exit $fail_task
else
    print_executor_logs
    exit 0
fi
