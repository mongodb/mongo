#!/bin/bash
set -euo pipefail

sdk_root="$(cd -- "${1%/*}/.." && pwd)"
test_dir="$(mktemp -d "${TEST_TMPDIR:-/tmp}/wasi-wrapper-test.XXXXXX")"
trap 'rm -rf -- "$test_dir"' EXIT

copy_sdk_tool() {
    local source_root="$1" tool="$2" destination="$3"
    local source="$source_root/bin/$tool"
    if [[ "$tool" == llvm-ar && -f "$source_root/bin/llvm-ar.sdk" ]]; then
        source="$source_root/bin/llvm-ar.sdk"
    fi
    cp -L -- "$source" "$destination"
    chmod a-x "$destination"
    [[ ! -x "$destination" ]]
}

# Generate the production entrypoints on Linux, and reproduce a Windows SDK
# materialization without executable bits. Never change the downloaded SDK.
fixture="$test_dir/sdk with spaces"
mkdir -p "$fixture/bin" "$test_dir/tools tmp"
for tool in clang-22 llvm-ar wasm-ld wasm-component-ld; do
    copy_sdk_tool "$sdk_root" "$tool" "$fixture/bin/$tool"
done
for wrapper in "${@:2}"; do
    cp -- "$wrapper" "$fixture/bin/"
done
ln -s -- "$sdk_root/lib" "$fixture/lib"
ln -s -- "$sdk_root/share" "$fixture/share"

clang="$fixture/bin/mongo-wasm32-wasip2-clang"
clangpp="$fixture/bin/mongo-wasm32-wasip2-clang++"
ar="$fixture/bin/mongo-llvm-ar"
export TMPDIR="$test_dir/tools tmp" TMP=Z:/b TEMP=Z:/b

assert_clean() {
    local staged
    shopt -s nullglob
    staged=("$TMPDIR"/mongo-wasi-*)
    if [[ ${#staged[@]} != 0 ]]; then
        printf 'Wrapper left temporary tools behind: %s\n' "${staged[@]}" >&2
        return 1
    fi
}

for tool in "$clang" "$clangpp" "$ar"; do
    PATH=/nonexistent LD_LIBRARY_PATH= "$tool" --version >/dev/null
    assert_clean
done

# Include builtin and C++ headers to verify the default resource directory and
# the clang++ driver name, then link with a non-executable SDK wasm-ld.
cat >"$test_dir/main.cpp" <<'SOURCE'
#include <stddef.h>
#include <stdarg.h>
#include <vector>
int main() { return std::vector<int>{1, 2}.size() != 2; }
SOURCE
PATH=/nonexistent "$clangpp" \
    --target=wasm32-wasip2 \
    -fno-exceptions \
    --sysroot="$fixture/share/wasi-sysroot" \
    -o "$test_dir/default-resource-dir.wasm" \
    "$test_dir/main.cpp"
[[ -s "$test_dir/default-resource-dir.wasm" ]]
assert_clean

PATH=/nonexistent "$clangpp" \
    --target=wasm32-wasip2 \
    -fwasm-exceptions \
    --sysroot="$fixture/share/wasi-sysroot" \
    -resource-dir="$fixture/lib/clang/22" \
    -L"$fixture/share/wasi-sysroot/lib/wasm32-wasip2/eh" \
    -lc++ \
    -lc++abi \
    -lwasi-emulated-signal \
    -lwasi-emulated-mman \
    -lwasi-emulated-process-clocks \
    -lwasi-emulated-getpid \
    -lunwind \
    -o "$test_dir/explicit-resource-dir.wasm" \
    "$test_dir/main.cpp"
[[ -s "$test_dir/explicit-resource-dir.wasm" ]]
assert_clean

# Also exercise the C entrypoint, the archiver, and TMP/TEMP normalization when
# TMPDIR is a Windows path that does not exist on the Linux worker.
echo 'int answer(void) { return 42; }' | PATH=/nonexistent TMPDIR=Z:/b "$clang" \
    --target=wasm32-wasip2 --sysroot="$fixture/share/wasi-sysroot" \
    -c -x c - -o "$test_dir/answer.o"
PATH=/nonexistent "$ar" rcs "$test_dir/answer.a" "$test_dir/answer.o"
[[ -s "$test_dir/answer.a" ]]
assert_clean

# IBM repositories keep the SDK archiver in llvm-ar.sdk and replace llvm-ar
# with a dispatcher. The fixture must use the payload so relocation does not
# break that dispatcher's sibling paths. Exercise this layout on every host.
ibm_sdk="$test_dir/ibm sdk"
ibm_fixture="$test_dir/ibm fixture"
mkdir -p "$ibm_sdk/bin" "$ibm_fixture/bin"
ln -s -- "$fixture/bin/llvm-ar" "$ibm_sdk/bin/llvm-ar.sdk"
printf '#!/bin/sh\nexit 99\n' >"$ibm_sdk/bin/llvm-ar"
copy_sdk_tool "$ibm_sdk" llvm-ar "$ibm_fixture/bin/llvm-ar"
cp -- "$ar" "$ibm_fixture/bin/mongo-llvm-ar"
ln -s -- "$sdk_root/lib" "$ibm_fixture/lib"
PATH=/nonexistent "$ibm_fixture/bin/mongo-llvm-ar" \
    rcs "$test_dir/ibm-answer.a" "$test_dir/answer.o"
[[ -s "$test_dir/ibm-answer.a" ]]
assert_clean

# A stub tool inspects the wrapper environment, including empty, unset, and
# inherited search paths, and verifies cleanup and status propagation on error.
cat >"$fixture/bin/clang-22" <<'STUB'
#!/bin/bash
set -euo pipefail
[[ "$PATH" != :* && "$PATH" != *: && "$PATH" != *::* ]]
[[ "$LD_LIBRARY_PATH" != :* && "$LD_LIBRARY_PATH" != *: && "$LD_LIBRARY_PATH" != *::* ]]
[[ "$TMPDIR" == "$TMP" && "$TMPDIR" == "$TEMP" && -d "$TMPDIR" ]]
[[ -x "${0%/*}/wasm-ld" ]]
[[ -x "${0%/*}/wasm-component-ld" ]]
[[ "$(stat -c %a "${0%/*}")" == 700 ]]
if [[ "${EXPECT_INHERITED:-}" == 1 ]]; then
    [[ "$PATH" == *:/sentinel/bin && "$LD_LIBRARY_PATH" == *:/sentinel/lib ]]
fi
if [[ "${WAIT_FOR_SIGNAL:-}" == 1 ]]; then
    printf '%s\n' "$BASHPID" > "$READY_FILE"
    trap 'exit 143' TERM
    while true; do sleep 0.1; done
fi
exit "${TOOL_STATUS:-0}"
STUB
chmod a-x "$fixture/bin/clang-22"
PATH= LD_LIBRARY_PATH= "$clang"
assert_clean
env -u PATH -u LD_LIBRARY_PATH "$clang"
assert_clean
EXPECT_INHERITED=1 PATH=/sentinel/bin LD_LIBRARY_PATH=/sentinel/lib "$clang"
assert_clean
status=0
TOOL_STATUS=42 PATH=/nonexistent "$clang" || status=$?
[[ "$status" == 42 ]]
assert_clean

# Terminating the wrapper must stop the child and remove its staged tools.
WAIT_FOR_SIGNAL=1 READY_FILE="$test_dir/ready" PATH=/nonexistent "$clang" &
wrapper_pid=$!
for attempt in {1..100}; do
    [[ -s "$test_dir/ready" ]] && break
    sleep 0.05
done
if [[ ! -s "$test_dir/ready" ]]; then
    kill -TERM "$wrapper_pid"
    wait "$wrapper_pid" || true
    echo 'Tool did not start before the signal-test deadline' >&2
    exit 1
fi
tool_pid="$(cat "$test_dir/ready")"
kill -TERM "$wrapper_pid"
status=0
wait "$wrapper_pid" || status=$?
[[ "$status" == 143 ]]
! kill -0 "$tool_pid" 2>/dev/null
assert_clean
