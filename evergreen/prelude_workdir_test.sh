#!/usr/bin/env bash

set -euo pipefail

source_helper="${TEST_SRCDIR}/${TEST_WORKSPACE}/evergreen/prelude_workdir.sh"
test_root="${TEST_TMPDIR}/prelude_workdir"
mkdir -p "${test_root}/src/evergreen"

run_helper() {
    local supplied_workdir="$1"
    local expected_status="$2"
    local output
    local status=0

    output=$(
        cd "${test_root}"
        evergreen_dir="${test_root}/src/evergreen"
        workdir="${supplied_workdir}"
        OS=""
        pwd_cygpath="$PWD"
        source "${source_helper}"
        printf '%s\n' "$workdir"
    ) || status=$?

    if [[ "$status" -ne "$expected_status" ]]; then
        echo "expected helper status ${expected_status}, got ${status}" >&2
        echo "$output" >&2
        return 1
    fi

    if [[ "$expected_status" -eq 0 && "$output" != "$supplied_workdir" ]]; then
        echo "helper changed the supplied workdir: ${output}" >&2
        return 1
    fi
}

canonical_workdir="${test_root}"
symlink_workdir="${TEST_TMPDIR}/prelude_workdir_link"
invalid_workdir="${TEST_TMPDIR}/prelude_workdir_other"
ln -s "${canonical_workdir}" "${symlink_workdir}"
mkdir -p "${invalid_workdir}"

# A symlinked Evergreen expansion is equivalent to the checkout path on POSIX hosts.
run_helper "${symlink_workdir}" 0

# An unrelated existing path must still be rejected.
run_helper "${invalid_workdir}" 1

windows_physical_root="${TEST_TMPDIR}/prelude_workdir_windows"
windows_logical_root="${TEST_TMPDIR}/prelude_workdir_windows_link"
mkdir -p "${windows_physical_root}/src/evergreen"
ln -s "${windows_physical_root}" "${windows_logical_root}"

# Windows+Cygwin keeps logical paths; this prevents POSIX canonicalization from
# changing that behavior.
windows_workdir_output=$(
    cd "${windows_logical_root}"
    evergreen_dir="${windows_logical_root}/src/evergreen"
    workdir=""
    OS="Windows_NT"
    cygpath() {
        printf '%s\n' "$2"
    }
    source "${source_helper}"
    printf '%s\n' "$workdir"
)

if [[ "${windows_workdir_output}" != "${windows_logical_root}" ]]; then
    echo "Windows helper did not preserve the logical workdir: ${windows_workdir_output}" >&2
    exit 1
fi
