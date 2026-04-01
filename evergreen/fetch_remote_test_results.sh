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

# Symlinks test logs from a test result into Evergreen's log ingestion folder.
function symlink_test_logs() {
    local build_dir='test.outputs/build/TestLogs'

    if [[ ! -d "$build_dir" ]]; then
        return
    fi

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

# Displays a formatted summary of test results.
function display_test_summary() {
    echo "================================================================================"
    echo "Test Results Summary"
    echo "================================================================================"
    echo "Target: ${test_label}"
    echo "Total Shards: ${#shard_names[@]}"
    echo "--------------------------------------------------------------------------------"

    # Create a sorted list of indices based on shard names
    local sorted_indices=()
    for i in "${!shard_names[@]}"; do
        sorted_indices+=("$i")
    done

    # Sort indices by extracting and comparing shard numbers
    IFS=$'\n' sorted_indices=($(
        for i in "${sorted_indices[@]}"; do
            local shard_num=$(echo "${shard_names[$i]}" | grep -oP 'shard_\K\d+$')
            echo "$shard_num $i"
        done | sort -n | cut -d' ' -f2
    ))

    for i in "${sorted_indices[@]}"; do
        local shard="${shard_names[$i]}"
        local status="${shard_statuses[$i]}"
        local test_counts="${shard_test_counts[$i]}"

        # Format status with color indicators
        case "$status" in
        "PASSED")
            echo "  ✓ $shard: PASSED ($test_counts tests passed)"
            ;;
        "FAILED")
            if [[ "$test_counts" == "0/0" ]]; then
                echo "  ✗ $shard: FAILED (no report generated)"
            else
                echo "  ✗ $shard: FAILED ($test_counts tests passed)"
            fi
            ;;
        "TIMEOUT")
            echo "  ⏱ $shard: TIMEOUT"
            ;;
        "NO_REPORT")
            echo "  ✗ $shard: NO REPORT (no tests may have been run)"
            ;;
        esac
    done

    echo "================================================================================"
    echo ""
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

    local total_tests=$(echo "$combined_report" | jq '.results | length')
    local failures=$(echo "$combined_report" | jq '.failures')

    echo ""
    echo "Combined Report: ${total_tests} tests, ${failures} failures"
    echo "Report written to: $combined_report_file"
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

# Print the contents of all *test.log files with headers per shard.
function print_executor_logs() {
    local log_files=$(find "${workdir}/results" -name '*test.log' -type f 2>/dev/null)

    if [[ -z "$log_files" ]]; then
        return
    fi

    # Sort log files by shard number
    local sorted_log_files=$(echo "$log_files" | while IFS= read -r log_file; do
        # Extract shard number from path (e.g., /workdir/results/foo/bar/shard_1/test.log -> 1)
        local shard_num=$(echo "$log_file" | grep -oP 'shard_\K\d+(?=/)')
        echo "$shard_num $log_file"
    done | sort -n | cut -d' ' -f2-)

    while IFS= read -r log_file; do
        # Extract shard name from path (e.g., /workdir/results/foo/bar/shard_1/test.log -> foo/bar/shard_1)
        local shard_path=$(echo "$log_file" | sed "s|${workdir}/results/||" | sed 's|/[^/]*$||')

        echo "================================================================================"
        echo "Shard $shard_path log:"
        echo "================================================================================"
        cat "$log_file"
        echo ""
        echo "================================================================================"
        echo ""
    done <<<"$sorted_log_files"
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
        write_test_failures_expansion
    elif is_failure "$test_result"; then
        is_failure_flag=1
        fail_task=1
        write_test_failures_expansion
    fi

    download_outputs "$test_result" "$is_failure_flag"
    unzip_outputs "$is_failure_flag"
    symlink_test_logs

    # Record shard information
    shard_names+=("$target_prefix")
    # Check if any report*.json files exist
    if compgen -G "test.outputs/report*.json" >/dev/null; then
        # Extract test counts from the report
        report_file=$(compgen -G "test.outputs/report*.json" | head -n 1)
        total_tests=$(jq '.results | length' "$report_file" 2>/dev/null || echo "0")
        failed_tests=$(jq '.results | map(select(.status == "fail" or .status == "timeout")) | length' "$report_file" 2>/dev/null || echo "0")
        passed_tests=$(jq '.results | map(select(.status == "pass")) | length' "$report_file" 2>/dev/null || echo "0")

        shard_test_counts+=("$passed_tests/$total_tests")

        if [[ "$is_timeout_flag" -eq 1 ]]; then
            shard_statuses+=("TIMEOUT")
        elif [[ "$is_failure_flag" -eq 1 ]]; then
            if [[ "$total_tests" -eq 0 ]]; then
                shard_statuses+=("NO_REPORT")
            else
                shard_statuses+=("FAILED")
            fi
        else
            shard_statuses+=("PASSED")
        fi
    else
        # No report file found - check if we have bazel-level status information
        if [[ "$is_timeout_flag" -eq 1 ]]; then
            shard_statuses+=("TIMEOUT")
            shard_test_counts+=("0/0")
        else
            shard_statuses+=("NO_REPORT")
            shard_test_counts+=("0/0")
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

print_executor_logs

display_test_summary

combine_metrics

failures=$(combine_reports)

write_bazel_invocation

# Check for system-level failures (TIMEOUT or NO_REPORT)
for status in "${shard_statuses[@]}"; do
    if [[ "$status" == "TIMEOUT" || "$status" == "NO_REPORT" ]]; then
        echo "Error: One or more shards had TIMEOUT or NO_REPORT status. Not all tests ran or were reported." >&2
        write_test_failures_expansion
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
    write_test_failures_expansion
fi

exit 0
