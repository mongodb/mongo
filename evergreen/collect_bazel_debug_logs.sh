#!/usr/bin/env bash
# Collects bazel debug logs (jstack files, command.log, java.log) and zips them
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"
. "$DIR/bazel_evergreen_shutils.sh"

set -o errexit
set -o pipefail

cd src

collect_bazel_debug_logs() {
    BAZEL_BINARY="$(bazel_evergreen_shutils::bazel_get_binary_path)"

    # Get output_base
    local ob
    ob="$(bazel_evergreen_shutils::bazel_output_base "$BAZEL_BINARY")" || {
        echo "Unable to get bazel output_base" >&2
        return 1
    }

    local zip_file="bazel-debug-logs.zip"

    echo "Collecting bazel debug logs into $zip_file" >&2

    # Check if zip command is available
    if ! command -v zip >/dev/null 2>&1; then
        echo "zip command not found; cannot create archive" >&2
        return 1
    fi

    # Create temporary list of files to zip
    local files_to_zip=()

    # Collect jstack files from current directory
    local jstack_files
    jstack_files=$(find . -maxdepth 1 -name "bazel_jstack_*.txt" -type f 2>/dev/null || true)
    if [[ -z "$jstack_files" ]]; then
        echo "No jstack files found; nothing to collect" >&2
        return 0
    fi

    while IFS= read -r file; do
        if [[ -n "$file" ]]; then
            files_to_zip+=("$file")
            echo "Found jstack file: $file" >&2
        fi
    done <<<"$jstack_files"

    # Collect command.log from output_base
    local command_log="${ob}/command.log"
    if [[ -f "$command_log" ]]; then
        files_to_zip+=("$command_log")
        echo "Found command.log: $command_log" >&2
    fi

    # Collect java.log from output_base
    local java_log="${ob}/java.log"
    if [[ -f "$java_log" ]]; then
        files_to_zip+=("$java_log")
        echo "Found java.log: $java_log" >&2
    fi

    # Check if we have any files to zip
    if [[ ${#files_to_zip[@]} -eq 0 ]]; then
        echo "No debug files found to collect" >&2
        return 0
    fi

    # Create the zip file
    echo "Creating archive with ${#files_to_zip[@]} file(s)..." >&2
    if zip -q -j "$zip_file" "${files_to_zip[@]}" 2>&1; then
        echo "Debug logs archived to $(pwd)/$zip_file" >&2
    else
        echo "Failed to create zip archive" >&2
        return 1
    fi
}

collect_bazel_debug_logs
