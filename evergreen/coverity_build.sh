#!/bin/env bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"
. "$DIR/bazel_evergreen_shutils.sh"

cd src

set -eo pipefail

covIdir="$workdir/covIdir"
if [ -d "$covIdir" ]; then
    echo "covIdir already exists, meaning idir extracted after download from S3"
else
    mkdir $workdir/covIdir
fi

activate_venv
export MONGO_WRAPPER_OUTPUT_ALL=1
BAZEL_BINARY="$(bazel_evergreen_shutils::bazel_get_binary_path)"
export BAZEL_BINARY
# number of parallel jobs to use for build.
# Even with scale=0 (the default), bc command adds decimal digits in case of multiplication. Division by 1 gives us a whole number with scale=0
coverity_config_dir="$workdir/coverity/config"
coverity_config_file="$coverity_config_dir/coverity_config.xml"

build_config="--config=local --build_atlas=True --compiler_type=gcc --opt=off --dbg=False --allocator=system --define=MONGO_VERSION=${version}"
bazel_cache="--output_user_root=$workdir/bazel_cache"
compiledb_target_pattern_file="$(mktemp "$workdir/install-core-compiledb-targets.XXXXXX")"
query_stderr_file="$(mktemp "$workdir/install-core-compiledb-query-stderr.XXXXXX")"
trap 'rm -f "$compiledb_target_pattern_file" "$query_stderr_file"' EXIT

echo "Generating compile_commands.json for Coverity capture"
echo "Resolving mongo_compiledb targets under //:install-core"
query_command=(
    "$BAZEL_BINARY"
    $bazel_cache
    cquery
    $build_config
    'attr("tags", ".*mongo_compiledb.*", deps(//:install-core))'
)
printf ' %q' "${query_command[@]}"
echo
if ! "${query_command[@]}" \
    2>"$query_stderr_file" | grep "//src/mongo" | awk '{print $1}' | sort -u >"$compiledb_target_pattern_file"; then
    echo "Failed to resolve mongo_compiledb targets under //:install-core"
    cat "$query_stderr_file"
    exit 1
fi

echo "Contents of $compiledb_target_pattern_file"
cat "$compiledb_target_pattern_file"

if [ ! -s "$compiledb_target_pattern_file" ]; then
    echo "No mongo_compiledb targets found under //:install-core"
    exit 1
fi

build_compiledb_command=(
    "$BAZEL_BINARY"
    $bazel_cache
    build
    $build_config
    --config=compiledb
    --target_pattern_file="$compiledb_target_pattern_file"
)
printf ' %q' "${build_compiledb_command[@]}"
echo
"${build_compiledb_command[@]}"

echo "Setting up clang-tidy IDE files"
"$BAZEL_BINARY" $bazel_cache run $build_config //:setup_clang_tidy

compiledb_output_base="$("$BAZEL_BINARY" $bazel_cache info output_base)"
repo_python=""
python_candidates=(
    "$compiledb_output_base/external/_main~setup_mongo_python_toolchains~py_host/dist/bin/python3"
    "$compiledb_output_base/external/py_host/dist/bin/python3"
    "$compiledb_output_base/external/_main~setup_mongo_python_toolchains~py_host/dist/python.exe"
    "$compiledb_output_base/external/py_host/dist/python.exe"
)
for candidate in "${python_candidates[@]}"; do
    if [ -x "$candidate" ]; then
        repo_python="$candidate"
        break
    fi
done
if [ -z "$repo_python" ]; then
    for candidate in "$compiledb_output_base"/external/*py_host*/dist/bin/python3 \
        "$compiledb_output_base"/external/*py_host*/dist/python.exe; do
        if [ -x "$candidate" ]; then
            repo_python="$candidate"
            break
        fi
    done
fi
echo "Resolved repo-rule python: $repo_python"
if [ -z "$repo_python" ] || [ ! -x "$repo_python" ]; then
    echo "Failed to resolve repo-rule python in Bazel output tree"
    exit 1
fi

mkdir -p "$coverity_config_dir"
if [ ! -f "$coverity_config_file" ]; then
    echo "Configuring Coverity compiler capture with default gcc template"
    "$workdir/coverity/bin/cov-configure" --gcc --config "$coverity_config_file"
fi

capture_command=(
    env
    "BUILD_WORKSPACE_DIRECTORY=$PWD"
    "VALIDATE_COMPILE_COMMANDS_RUN_ALL=1"
    "VALIDATE_COMPILE_COMMANDS_OUT_DIR=$workdir/validate_compile_commands_out"
    "$repo_python"
    evergreen/validate_compile_commands.py
)

echo "Running Coverity capture via compile_commands.json replay"
printf ' %q' "${capture_command[@]}"
echo
if $workdir/coverity/bin/cov-build --dir "$covIdir" --config "$coverity_config_file" --verbose 0 --return-emit-failures --parse-error-threshold=99 \
    "${capture_command[@]}"; then
    echo "cov-build was successful"
else
    ret=$?
    echo "cov-build failed with exit code $ret"
    if [ -f "$covIdir/replay-log.txt" ]; then
        cat "$covIdir/replay-log.txt"
    fi
    exit $ret
fi
