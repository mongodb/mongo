#!/usr/bin/env bash

set -euo pipefail

if [[ "$#" -ne 3 ]]; then
    echo "Usage: gdb-apply-index <index-bundle> <input-binary> <output-binary>" >&2
    exit 1
fi

index_bundle="$1"
input_binary="$2"
output_binary="$3"
objcopy="${OBJCOPY:?OBJCOPY must be set}"

if [[ -f "${index_bundle}/no_index" ]]; then
    cp "${input_binary}" "${output_binary}"
    exit 0
fi

if [[ -f "${index_bundle}/gdb_index" ]]; then
    index_section=".gdb_index"
    index_file="${index_bundle}/gdb_index"
elif [[ -f "${index_bundle}/debug_names" ]]; then
    index_section=".debug_names"
    index_file="${index_bundle}/debug_names"
else
    echo "Could not find a GDB index in ${index_bundle}." >&2
    exit 1
fi

temporary_directory="$(mktemp -d)"
trap 'rm -rf "${temporary_directory}"' EXIT

objcopy_arguments=(
    --add-section "${index_section}=${index_file}"
    --set-section-flags "${index_section}=readonly"
)

if [[ -f "${index_bundle}/debug_str" ]]; then
    debug_str_error="${temporary_directory}/debug_str.err"
    debug_str_merge="${temporary_directory}/debug_str.merge"

    if "${objcopy}" --dump-section ".debug_str=${temporary_directory}/debug_str" "${input_binary}" /dev/null 2>"${debug_str_error}"; then
        cat "${temporary_directory}/debug_str" "${index_bundle}/debug_str" >"${debug_str_merge}"
        objcopy_arguments+=(--update-section ".debug_str=${debug_str_merge}")
    elif grep -q "can't dump section '.debug_str' - it does not exist" "${debug_str_error}"; then
        cp "${index_bundle}/debug_str" "${debug_str_merge}"
        objcopy_arguments+=(
            --add-section ".debug_str=${debug_str_merge}"
            --set-section-flags .debug_str=readonly
        )
    else
        cat "${debug_str_error}" >&2
        exit 1
    fi
fi

"${objcopy}" "${objcopy_arguments[@]}" "${input_binary}" "${output_binary}"
