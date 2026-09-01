#!/bin/bash
# Run the release package test with a temporary RHEL subscription.

set -o errexit
set -o nounset
set -o pipefail

cleanup() {
    local exit_code=$?

    if ! ./src/buildscripts/package_test_register.sh -a remove; then
        echo "Failed to remove the RHEL subscription" >&2
        if [[ $exit_code -eq 0 ]]; then
            exit_code=1
        fi
    fi

    exit "$exit_code"
}

trap cleanup EXIT

./src/buildscripts/package_test_register.sh -a add
./src/evergreen/run_python_script.sh \
    buildscripts/package_test/package_test.py \
    release \
    --skip-system-library-check \
    --evg-project="${project:-}"
