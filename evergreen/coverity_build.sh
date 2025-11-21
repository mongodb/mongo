#!/bin/env bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

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
# number of parallel jobs to use for build.
# Even with scale=0 (the default), bc command adds decimal digits in case of multiplication. Division by 1 gives us a whole number with scale=0
bazel_jobs=$(bc <<<"$(grep -c '^processor' /proc/cpuinfo) * .85 / 1")
build_config="--config=local --jobs=$bazel_jobs --build_atlas=True --compiler_type=gcc --opt=off --dbg=False --allocator=system --define=MONGO_VERSION=${version}"
bazel_query='mnemonic("CppCompile|LinkCompile", filter(//src/mongo, deps(//:install-core)) except //src/mongo/db/modules/enterprise/src/streams/third_party/...)'
bazel_cache="--output_user_root=$workdir/bazel_cache"

python bazel/coverity/generate_coverity_targets.py --bazel_executable="bazel" --bazel_cache=$bazel_cache --bazel_query="$bazel_query" $build_config --noinclude_artifacts
bazel $bazel_cache build $build_config --build_tag_filters=gen_source //src/...

buildCommand="bazel \
    $bazel_cache \
    build \
    $build_config \
    --target_pattern_file=coverity_targets.list"
echo Building $buildCommand
cat coverity_targets.list
if ! $workdir/coverity/bin/cov-build --dir "$covIdir" --verbose 0 -j $bazel_jobs --return-emit-failures --parse-error-threshold=99 --bazel \
    $buildCommand; then
    ret=$?
    echo "cov-build failed with exit code $ret"
    cat $covIdir/replay-log.txt
    exit $ret
else
    echo "cov-build was successful"
fi
