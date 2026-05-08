# Shared bash helpers for processing bazel resmoke test outputs in Evergreen result tasks.
#
# Required environment variables (set by callers):
# * ${workdir}    - The Evergreen workdir.
# * ${test_label} - The bazel test target label (e.g. //buildscripts/resmokeconfig:core).

# Converts a bazel test label into the path-prefix convention used in ${workdir}/results.
# Example: //buildscripts/resmokeconfig:core -> buildscripts/resmokeconfig/core
function bazel_test_results::label_to_prefix() {
    local label="$1"
    label="${label#//}"
    label="${label//:/\/}"
    echo "${label}"
}

# Symlinks test logs from a per-shard test.outputs/build/TestLogs directory into Evergreen's
# log ingestion folder. Must be invoked with the per-shard results directory as cwd.
function bazel_test_results::symlink_test_logs() {
    local -r build_dir='test.outputs/build/TestLogs'

    if [[ ! -d "${build_dir}" ]]; then
        echo "No test logs directory found at ${build_dir}, skipping symlink."
        return
    fi

    find "${build_dir}" -type f | while read -r file; do
        rel_path="${file#${build_dir}/}"
        target_path="${workdir}/build/TestLogs/${rel_path}"
        target_dir=$(dirname "${target_path}")

        mkdir -p "${target_dir}"

        abs_file=$(realpath "${file}")
        ln -sf "${abs_file}" "${target_path}"
    done
}

# Combines all resmoke OTel telemetry under ${workdir}/results into batched files under
# ${workdir}/build/OTelTraces. Evergreen processes slowly when there are many small files.
# However, files are kept under 4MB since that is the maximum size that
# Evergreen will send the the trace collector without re-batching them.
function bazel_test_results::combine_metrics() {
    local -r output_dir="${workdir}/build/OTelTraces"
    mkdir -p "${output_dir}"

    local max_size=$((4 * 1024 * 1024))
    local file_counter=0
    local current_size=0
    local current_output="${output_dir}/metrics.json"

    >"${current_output}"

    find "${workdir}/results" -wholename '*metrics/metrics*.json' -type f -print0 | while IFS= read -r -d '' file; do
        local file_size=$(stat -c%s "${file}")
        local newline_size=1

        if ((current_size + file_size + newline_size > max_size && current_size > 0)); then
            ((file_counter++))
            current_output="${output_dir}/metrics_${file_counter}.json"
            current_size=0
            >"${current_output}"
        fi

        cat "${file}" >>"${current_output}"
        echo "" >>"${current_output}"

        current_size=$((current_size + file_size + newline_size))
    done
}

# Combines all resmoke report JSONs into a single ${workdir}/report.json for attach.results.
function bazel_test_results::combine_reports() {
    local -r report_files=$(find "${workdir}" -name 'report*.json' -type f 2>/dev/null)

    if [[ -z "${report_files}" ]]; then
        echo 'No report.json files found'
        return
    fi

    local -r combined_report=$(echo "${report_files}" | xargs jq -s '
        {
            results: map(.results // []) | add,
            failures: (map(.results // []) | add | map(select(.status == "fail" or .status == "timeout")) | length)
        }
    ')

    local -r combined_report_file="${workdir}/report.json"
    echo "${combined_report}" >"${combined_report_file}"

    local -r total_tests=$(echo "$combined_report" | jq '.results | length')
    local -r failures=$(echo "$combined_report" | jq '.failures')

    echo ""
    echo "Combined Report: ${total_tests} tests, ${failures} failures"
    echo "Report written to: ${combined_report_file}"
}

# Writes a YAML file indicating that test failures exist (consumed by expansions.update).
function bazel_test_results::write_test_failures_expansion() {
    local -r output_file="${workdir}/results/test_failures_exist.yml"
    mkdir -p "$(dirname "${output_file}")"
    echo "test_failures_exist: true" >"${output_file}"
}

# Prints all *test.log files with per-shard headers, ordered by shard number.
function bazel_test_results::print_executor_logs() {
    local -r log_files=$(find "${workdir}/results" -name '*test.log' -type f 2>/dev/null)

    if [[ -z "${log_files}" ]]; then
        return
    fi

    local -r sorted_log_files=$(echo "${log_files}" | while IFS= read -r log_file; do
        local shard_num=$(echo "${log_file}" | grep -oP 'shard_\K\d+(?=/)')
        echo "${shard_num} ${log_file}"
    done | sort -n | cut -d' ' -f2-)

    while IFS= read -r log_file; do
        local shard_path=$(echo "${log_file}" | sed "s|${workdir}/results/||" | sed 's|/[^/]*$||')

        echo "================================================================================"
        echo "Shard ${shard_path} log:"
        echo "================================================================================"
        cat "${log_file}"
        echo ""
        echo "================================================================================"
        echo ""
    done <<<"${sorted_log_files}"
}

# Displays a formatted summary of test results. Caller passes parallel arrays by name.
# Usage: bazel_test_results::display_test_summary shard_names_var shard_statuses_var shard_test_counts_var
function bazel_test_results::display_test_summary() {
    local -n _names="${1}"
    local -n _statuses="${2}"
    local -n _counts="${3}"

    echo "================================================================================"
    echo "Test Results Summary"
    echo "================================================================================"
    echo "Target: ${test_label}"
    echo "Total Shards: ${#_names[@]}"
    echo "--------------------------------------------------------------------------------"

    local sorted_indices=()
    for i in "${!_names[@]}"; do
        sorted_indices+=("$i")
    done

    IFS=$'\n' sorted_indices=($(
        for i in "${sorted_indices[@]}"; do
            local shard_num=$(echo "${_names[$i]}" | grep -oP 'shard_\K\d+$')
            echo "${shard_num} ${i}"
        done | sort -n | cut -d' ' -f2
    ))

    for i in "${sorted_indices[@]}"; do
        local shard="${_names[$i]}"
        local status="${_statuses[$i]}"
        local test_counts="${_counts[$i]}"

        case "${status}" in
        "PASSED")
            echo "  ✓ ${shard}: PASSED (${test_counts} tests passed)"
            ;;
        "FAILED")
            if [[ "${test_counts}" == "0/0" ]]; then
                echo "  ✗ ${shard}: FAILED (no report generated)"
            else
                echo "  ✗ ${shard}: FAILED (${test_counts} tests passed)"
            fi
            ;;
        "TIMEOUT")
            echo "  ⏱ ${shard}: TIMEOUT"
            ;;
        "NO_REPORT")
            echo "  ✗ ${shard}: NO REPORT (no tests may have been run)"
            ;;
        esac
    done

    echo "================================================================================"
    echo ""
}

# Reads a single test result's report.json and appends the corresponding entries to the
# parallel summary arrays passed by name. Must be invoked with the shard's results dir as cwd.
# Usage: bazel_test_results::record_shard_status <shard_path> <is_failure_flag> <is_timeout_flag>
#                                                names_var statuses_var counts_var
# Returns 0 if the shard had a report.json, 1 otherwise.
function bazel_test_results::record_shard_status() {
    local -r shard_path="$1"
    local -r is_failure_flag="$2"
    local -r is_timeout_flag="$3"
    local -n _names="${4}"
    local -n _statuses="${5}"
    local -n _counts="${6}"

    _names+=("${shard_path}")

    local report_file
    report_file=$(compgen -G "test.outputs/report*.json" | head -n 1)
    if [[ -n "${report_file}" ]]; then
        local total_tests failed_tests passed_tests
        IFS=$'\t' read -r total_tests failed_tests passed_tests < <(
            jq -r '[
                (.results | length),
                (.results | map(select(.status == "fail" or .status == "timeout")) | length),
                (.results | map(select(.status == "pass")) | length)
            ] | @tsv' "${report_file}" 2>/dev/null || printf "0\t0\t0\n"
        )
        total_tests=${total_tests:-0}
        failed_tests=${failed_tests:-0}
        passed_tests=${passed_tests:-0}

        _counts+=("${passed_tests}/${total_tests}")

        if [[ "${is_timeout_flag}" -eq 1 ]]; then
            _statuses+=("TIMEOUT")
        elif [[ "${is_failure_flag}" -eq 1 || "${failed_tests}" -gt 0 ]]; then
            if [[ "${total_tests}" -eq 0 ]]; then
                _statuses+=("NO_REPORT")
            else
                _statuses+=("FAILED")
            fi
        else
            _statuses+=("PASSED")
        fi
        return 0
    else
        if [[ "${is_timeout_flag}" -eq 1 ]]; then
            _statuses+=("TIMEOUT")
            _counts+=("0/0")
        else
            _statuses+=("NO_REPORT")
            _counts+=("0/0")
        fi
        return 1
    fi
}
