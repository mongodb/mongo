#!/bin/bash
# Windows cross builds download the Linux WASI SDK on a Windows client but run
# its tools on Linux workers. Downloaded tools can lose execute permissions, and
# actions can inherit Windows paths. This wrapper makes the SDK runnable on
# Linux while preserving the compiler's driver and header lookup behavior.
set -euo pipefail

# The repository rule and test fixture substitute the SDK tool and driver name.
tool_name="@MONGO_WASI_REAL_TOOL@"
argv0="@MONGO_WASI_ARGV0@"
this_path="${BASH_SOURCE[0]}"
this_dir="${this_path%/*}"
if [[ "${this_dir}" == "${this_path}" ]]; then
    this_dir="."
fi
raw_dir="${this_dir}"
this_dir="$(cd -- "${this_dir}" && pwd)"
# Bazel actions may intentionally provide a tool-only PATH. The wrapper needs
# both the downloaded SDK's tools and these host utilities to make a Linux SDK
# executable. Append inherited search paths only when nonempty so an empty
# entry does not add the action's working directory to the search path.
export PATH="${this_dir}:/usr/bin:/bin${PATH:+:${PATH}}"
real_tool="${this_dir}/${tool_name}"
export LD_LIBRARY_PATH="${this_dir}/../lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
# Windows Evergreen hosts pass TMP/TEMP=Z:/b through --action_env, which reaches
# Linux RBE workers verbatim. LLVM consults TMPDIR, TMP, then TEMP for temporary
# files, so any of them naming a missing directory breaks linking with "unable
# to make temporary file". Point all of them at a directory that exists here.
tmp_dir="${TMPDIR:-}"
if [[ ! -d "${tmp_dir}" || ! -w "${tmp_dir}" ]]; then
    tmp_dir=/tmp
fi
export TMPDIR="${tmp_dir}" TMP="${tmp_dir}" TEMP="${tmp_dir}"
# Use a private, atomically created directory and keep it alive until the tool
# exits. A parent shell is needed to clean up after both success and failure.
stage_dir="$(mktemp -d -- "$TMPDIR/mongo-wasi-${argv0}.XXXXXX")"
trap 'rm -rf -- "$stage_dir"' EXIT
tool_pid=""
terminate() {
    trap '' HUP INT TERM
    if [[ -n "$tool_pid" ]]; then
        kill -s "$1" "$tool_pid" 2>/dev/null || true
        wait "$tool_pid" 2>/dev/null || true
    fi
    exit "$2"
}
trap 'terminate HUP 129' HUP
trap 'terminate INT 130' INT
trap 'terminate TERM 143' TERM
tmp_tool="$stage_dir/$argv0"
cp -- "${real_tool}" "${tmp_tool}"
chmod u+x "${tmp_tool}"
exec_args=()
if [[ "$tool_name" == clang-22 ]]; then
    # LLD selects its driver from the executable name. Keep wasm-ld's
    # basename when staging it. Preview2 also uses wasm-component-ld,
    # which Clang finds alongside wasm-ld using the -B search prefix.
    for linker in wasm-ld wasm-component-ld; do
        cp -- "$this_dir/$linker" "$stage_dir/$linker"
        chmod u+x "$stage_dir/$linker"
    done

    # Clang's builtin headers (stddef.h, stdarg.h, ...) live in the resource
    # dir. Derive it from the wrapper's own invoked path and strip the
    # action's working directory prefix so the dir stays relative and free of
    # ".." components: a clean relative resource dir makes clang report
    # relative header paths, which bazel accepts for sandboxed and
    # persistent-container compiles. (A hardcoded canonical repo name broke
    # on bazel 9's repo-name mangling; an absolute or ".."-containing dir
    # makes clang canonicalize paths through the sandbox symlinks and emit
    # absolute paths in the .d file, which bazel rejects.)
    resource_dir="${raw_dir%/*}/lib/clang/22"
    case "$resource_dir" in
    "$PWD"/*) resource_dir="${resource_dir#"$PWD"/}" ;;
    esac
    exec_args=(-resource-dir "$resource_dir" "-B$stage_dir")
fi
(
    trap - HUP INT TERM
    exec -a "$argv0" "${tmp_tool}" "${exec_args[@]}" "$@"
) <&0 &
tool_pid=$!
wait "$tool_pid"
