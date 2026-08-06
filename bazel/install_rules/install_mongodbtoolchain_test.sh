#!/usr/bin/env bash

set -euo pipefail

readonly runfiles_dir="$TEST_TMPDIR/runfiles"
readonly install_dir="$TEST_TMPDIR/install"
readonly script_path="$TEST_SRCDIR/$TEST_WORKSPACE/bazel/install_rules/install_mongodbtoolchain.sh"

mkdir -p \
    "$runfiles_dir/mongo_toolchain_v5/v5/bin" \
    "$runfiles_dir/mongo_toolchain_v5/stow/gcc-v5" \
    "$runfiles_dir/gdb_v5/v5/bin" \
    "$runfiles_dir/gdb_v5/stow/gdb-v5" \
    "$runfiles_dir/py_host/dist/bin"

printf 'mongo gcc\n' >"$runfiles_dir/mongo_toolchain_v5/v5/bin/gcc"
printf 'mongo marker\n' >"$runfiles_dir/mongo_toolchain_v5/stow/gcc-v5/marker"
printf 'gdb\n' >"$runfiles_dir/gdb_v5/v5/bin/gdb"
printf 'gdb marker\n' >"$runfiles_dir/gdb_v5/stow/gdb-v5/marker"
printf 'python\n' >"$runfiles_dir/py_host/dist/bin/python3"
printf 'python marker\n' >"$runfiles_dir/py_host/dist/python-marker"

RUNFILES_DIR="$runfiles_dir" \
    MONGODB_TOOLCHAIN_INSTALL_DIR="$install_dir" \
    "$script_path"

test -f "$install_dir/v5/bin/gcc"
test -f "$install_dir/v5/bin/gdb"
test -f "$install_dir/stow/gcc-v5/marker"
test -f "$install_dir/stow/gdb-v5/marker"
test -f "$install_dir/v5/python-marker"

grep -q '^mongo gcc$' "$install_dir/v5/bin/gcc"
grep -q '^gdb$' "$install_dir/v5/bin/gdb"
grep -q '^python marker$' "$install_dir/v5/python-marker"

set +e
mkdir_failure_path="$TEST_TMPDIR/mkdir-failure"
printf 'not a directory\n' >"$mkdir_failure_path"
mkdir_failure_output="$TEST_TMPDIR/mkdir-failure-output"
RUNFILES_DIR="$runfiles_dir" \
    MONGODB_TOOLCHAIN_INSTALL_DIR="$mkdir_failure_path/child" \
    "$script_path" >"$mkdir_failure_output" 2>&1
mkdir_failure_status=$?
set -e

test "$mkdir_failure_status" -ne 0
grep -q "Could not create install directory: $mkdir_failure_path/child" "$mkdir_failure_output"
