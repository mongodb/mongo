#!/usr/bin/env bash

set -euo pipefail

runfiles_root="${RUNFILES_DIR:-${TEST_SRCDIR}/${TEST_WORKSPACE}}"
gdb_toolchain_dir=""

for candidate in \
    "${runfiles_root}/_main/gdb-toolchain" \
    "${runfiles_root}/${TEST_WORKSPACE}/gdb-toolchain" \
    "${runfiles_root}/gdb-toolchain"; do
    if [[ -d "${candidate}" ]]; then
        gdb_toolchain_dir="${candidate}"
        break
    fi
done

if [[ -z "${gdb_toolchain_dir}" ]]; then
    echo "Could not locate gdb-toolchain in ${runfiles_root}" >&2
    exit 1
fi

test -x "${gdb_toolchain_dir}/v5/bin/gdb"
test -x "${gdb_toolchain_dir}/v5/bin/llvm-readelf"
test -x "${gdb_toolchain_dir}/v5/bin/llvm-objcopy"
test ! -L "${gdb_toolchain_dir}/v5/bin/gdb"
test ! -L "${gdb_toolchain_dir}/v5/bin/llvm-readelf"
test ! -L "${gdb_toolchain_dir}/v5/bin/llvm-objcopy"
test -f "${gdb_toolchain_dir}/stow/gdb-v5/bin/gdb"

printers="$(find "${gdb_toolchain_dir}/stow/gcc-v5/share" -path '*/python/libstdcxx/v6/printers.py' -print -quit)"
test -n "${printers}"
