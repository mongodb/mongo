#!/bin/bash

# Run from build folder

folder="./tools/tsan_playground"
timeout_seconds=5

for exe in "$folder"/tsan_playground_*; do
    full_path=$(realpath "$exe")
    echo " === Running $full_path ..."

    output=$(timeout "$timeout_seconds" "$exe" 2>&1)
    exit_code=$?

    label=$(echo "$output" | grep "Implementation:" | sed 's/^Implementation: //')
    warnings=$(echo "$output" | grep "SUMMARY: ThreadSanitizer:")

    if [ $exit_code -eq 124 ]; then
        echo "[Timeout] $exe exceeded $timeout_seconds seconds."
        continue
    fi

    [ $exit_code -eq 0 ] && status="[Completed]" || status="[Error]"
    echo "$status Results for $exe: (ERROR_CODE $exit_code)"
    echo "Implementation Label: $label"
    [ -n "$warnings" ] && echo "$warnings" || echo "No warnings detected."
    echo
done
