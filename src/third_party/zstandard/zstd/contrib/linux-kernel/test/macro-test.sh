#!/usr/bin/env sh

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
INCLUDE_DIR="$SCRIPT_DIR/../linux/include"
LIB_DIR="$SCRIPT_DIR/../linux/lib"


print() {
    printf '%b' "${*}"
}

println() {
    printf '%b\n' "${*}"
}

die() {
    println "$@" 1>&2
    exit 1
}

test_not_present() {
    print "Testing that '$1' is not present... "
    grep -r $1 "$INCLUDE_DIR" "$LIB_DIR" && die "Fail!"
    println "Okay"
}

println "This test checks that the macro removal process worked as expected"
println "If this test fails, then freestanding.py wasn't able to remove one of these"
println "macros from the source code completely. You'll either need to rewrite the check"
println "or improve freestanding.py."
println ""

test_not_present "ZSTD_NO_INTRINSICS"
test_not_present "ZSTD_NO_UNUSED_FUNCTIONS"
test_not_present "ZSTD_LEGACY_SUPPORT"
test_not_present "STATIC_BMI2"
test_not_present "ZSTD_DLL_EXPORT"
test_not_present "ZSTD_DLL_IMPORT"
test_not_present "__ICCARM__"
test_not_present "_MSC_VER"
test_not_present "_WIN32"
test_not_present "__linux__"
