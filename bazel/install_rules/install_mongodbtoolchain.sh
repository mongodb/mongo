#!/usr/bin/env bash

set -euo pipefail
shopt -s globstar nullglob

readonly DEFAULT_INSTALL_DIR="/opt/mongodbtoolchain"

usage() {
    cat <<'EOF'
Usage: bazel run //:install-local-toolchain -- [INSTALL_DIR]

Installs the MongoDB Bazel toolchains into /opt/mongodbtoolchain by default.
Set MONGODB_TOOLCHAIN_INSTALL_DIR or pass INSTALL_DIR to use a different
destination, which is useful for local validation.
EOF
}

if [[ $# -gt 1 ]]; then
    usage >&2
    exit 1
fi

install_dir="${MONGODB_TOOLCHAIN_INSTALL_DIR:-${1:-$DEFAULT_INSTALL_DIR}}"

runfiles_manifest="${RUNFILES_MANIFEST_FILE:-${0}.runfiles_manifest}"
runfiles_dir="${RUNFILES_DIR:-${0}.runfiles}"

fail() {
    echo "ERROR: $*" >&2
    exit 1
}

progress() {
    echo "INFO: $*" >&2
}

rlocation_path() {
    local logical_path="$1"
    local manifest_match=""
    local manifest_key
    local manifest_value
    local candidates=()

    if [[ -f "$runfiles_manifest" ]]; then
        while IFS=' ' read -r manifest_key manifest_value; do
            if [[ "$manifest_key" == "$logical_path" ]]; then
                printf '%s\n' "$manifest_value"
                return 0
            fi
            if [[ "$manifest_key" == *"$logical_path" ]]; then
                if [[ -n "$manifest_match" ]]; then
                    echo "Multiple manifest entries matched $logical_path" >&2
                    return 1
                fi
                manifest_match="$manifest_value"
            fi
        done <"$runfiles_manifest"

        if [[ -n "$manifest_match" ]]; then
            printf '%s\n' "$manifest_match"
            return 0
        fi
    fi

    if [[ -d "$runfiles_dir" && -e "$runfiles_dir/$logical_path" ]]; then
        printf '%s\n' "$runfiles_dir/$logical_path"
        return 0
    fi

    if [[ -d "$runfiles_dir" ]]; then
        candidates=("$runfiles_dir"/**/"$logical_path")
        if [[ "${#candidates[@]}" -eq 1 ]]; then
            printf '%s\n' "${candidates[0]}"
            return 0
        fi
        if [[ "${#candidates[@]}" -gt 1 ]]; then
            echo "Multiple runfiles matched $logical_path" >&2
            return 1
        fi
    fi

    return 1
}

require_runfile() {
    local logical_path="$1"
    local resolved_path

    resolved_path="$(rlocation_path "$logical_path")" || return 1
    if [[ -z "$resolved_path" ]]; then
        return 1
    fi

    printf '%s\n' "$resolved_path"
}

repo_root_from_anchor() {
    local logical_path="$1"
    local levels_up="$2"
    local anchor_path
    local repo_root
    local level

    anchor_path="$(require_runfile "$logical_path")" || return 1
    repo_root="$anchor_path"

    for ((level = 0; level < levels_up; level++)); do
        repo_root="$(dirname "$repo_root")"
    done

    printf '%s\n' "$repo_root"
}

copy_tree_contents() {
    local source_dir="$1"
    local destination_dir="$2"
    local cp_log
    local cp_output

    if [[ ! -d "$source_dir" ]]; then
        fail "Expected directory does not exist: $source_dir"
    fi

    mkdir -p "$destination_dir"
    cp_log="$(mktemp)"
    if ! cp -a --no-preserve=ownership "$source_dir/." "$destination_dir/" 2>"$cp_log"; then
        cp_output="$(<"$cp_log")"
        rm -f "$cp_log"
        if [[ -n "$cp_output" ]]; then
            printf '%s\n' "$cp_output" >&2
        fi
        print_cleanup_guidance "$destination_dir"
        exit 1
    fi
    rm -f "$cp_log"
}

copy_repo_directories() {
    local repo_root="$1"
    local destination_root="$2"
    local entry
    local entry_name

    mkdir -p "$destination_root"

    for entry in "$repo_root"/*; do
        [[ -d "$entry" ]] || continue
        entry_name="$(basename "$entry")"
        progress "Copying $entry_name into $destination_root"
        copy_tree_contents "$entry" "$destination_root/$entry_name"
    done
}

append_managed_repo_paths() {
    local repo_root="$1"
    local destination_root="$2"
    local entry
    local entry_name

    for entry in "$repo_root"/*; do
        [[ -d "$entry" ]] || continue
        entry_name="$(basename "$entry")"
        managed_paths["$destination_root/$entry_name"]=1
    done
}

build_cleanup_command() {
    local -n cleanup_paths_ref="$1"
    local command="sudo rm -rf"
    local path
    local quoted_path

    for path in "${cleanup_paths_ref[@]}"; do
        printf -v quoted_path '%q' "$path"
        command+=" $quoted_path"
    done

    printf '%s\n' "$command"
}

collect_existing_managed_paths() {
    local -n cleanup_paths_ref="$1"
    local managed_path

    for managed_path in "${!managed_paths[@]}"; do
        [[ -e "$managed_path" ]] || continue
        cleanup_paths_ref+=("$managed_path")
    done

    if [[ "${#cleanup_paths_ref[@]}" -eq 0 ]]; then
        cleanup_paths_ref+=("$install_dir")
    fi

    IFS=$'\n' cleanup_paths_ref=($(printf '%s\n' "${cleanup_paths_ref[@]}" | sort -u))
    unset IFS
}

print_cleanup_guidance() {
    local -a blocked_paths=("$@")
    local -a cleanup_paths=()
    local cleanup_command

    collect_existing_managed_paths cleanup_paths
    cleanup_command="$(build_cleanup_command cleanup_paths)"

    {
        echo "ERROR: Existing MongoDB toolchain files are not writable and would block installation."
        echo "Blocked paths:"
        printf '  %s\n' "${blocked_paths[@]}"
        echo
        echo "Remove the conflicting install paths and rerun:"
        printf '  %s\n' "$cleanup_command"
        printf '  bazel run //:install-local-toolchain -- %q\n' "$install_dir"
    } >&2
}

check_existing_install_conflicts() {
    local -a cleanup_paths=()
    local -a blocked_paths=()
    local managed_path
    local first_blocked_path

    for managed_path in "${!managed_paths[@]}"; do
        [[ -e "$managed_path" ]] || continue

        first_blocked_path="$(find -L "$managed_path" ! -w -print -quit 2>/dev/null || true)"
        if [[ -z "$first_blocked_path" && -w "$managed_path" ]]; then
            continue
        fi

        cleanup_paths+=("$managed_path")
        if [[ -n "$first_blocked_path" ]]; then
            blocked_paths+=("$first_blocked_path")
        else
            blocked_paths+=("$managed_path")
        fi
    done

    if [[ "${#cleanup_paths[@]}" -eq 0 ]]; then
        return 0
    fi

    IFS=$'\n' blocked_paths=($(printf '%s\n' "${blocked_paths[@]}" | sort -u))
    unset IFS

    print_cleanup_guidance "${blocked_paths[@]}"
    exit 1
}

if ! mkdir -p "$install_dir"; then
    fail "Could not create install directory: $install_dir"
fi
if [[ ! -w "$install_dir" ]]; then
    fail "Install directory is not writable: $install_dir"
fi

progress "Resolving MongoDB toolchain inputs"
mongo_toolchain_root="$(repo_root_from_anchor "mongo_toolchain_v5/v5/bin/gcc" 3)" || fail "Could not resolve the Mongo C++ toolchain runfiles"
gdb_toolchain_root="$(repo_root_from_anchor "gdb_v5/v5/bin/gdb" 3)" || fail "Could not resolve the Mongo GDB toolchain runfiles"
python_toolchain_root="$(repo_root_from_anchor "py_host/dist/bin/python3" 3)" || fail "Could not resolve the host Python toolchain runfiles"

declare -A managed_paths=()
append_managed_repo_paths "$mongo_toolchain_root" "$install_dir"
append_managed_repo_paths "$gdb_toolchain_root" "$install_dir"
managed_paths["$install_dir/v5"]=1
managed_paths["$install_dir/stow/python3-v5"]=1

progress "Checking existing install for conflicts"
check_existing_install_conflicts

progress "Installing Mongo C++ toolchain into $install_dir"
copy_repo_directories "$mongo_toolchain_root" "$install_dir"
progress "Installing Mongo GDB toolchain into $install_dir"
copy_repo_directories "$gdb_toolchain_root" "$install_dir"
progress "Installing host Python toolchain into $install_dir/v5"
copy_tree_contents "$python_toolchain_root/dist" "$install_dir/v5"

echo "Installed MongoDB toolchains into $install_dir"
