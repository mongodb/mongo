# Usage:
#   bazel_compile [arguments]
#
# Required environment variables:
# * ${targets} - List of build targets
#
# Optional environment variable(s):
# * ${args} - List of additional Bazel arguments (e.g.: "--config=clang-tidy")

# Needed for evergreen scripts that use evergreen expansions and utility methods.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose
set -o pipefail

. "$DIR/bazel_evergreen_shutils.sh"

bazel_evergreen_shutils::activate_and_cd_src
bazel_evergreen_shutils::export_ssl_paths_if_needed

# if build_patch_id is passed, try to download binaries from specified
# evergreen patch.
build_patch_id="${build_patch_id:-${reuse_compile_from}}"
if [[ -n "${build_patch_id}" ]]; then
    echo "build_patch_id detected, trying to skip task"

    # On windows we change the extension to zip
    if [[ -z "${ext:-}" ]]; then ext="tgz"; fi
    extra_db_contrib_args=""

    # get the platform of the dist archive. This is needed if
    # db-contrib-tool cannot autodetect the platform of the ec2 instance.
    regex='MONGO_DISTMOD=([a-z0-9]*)'
    if [[ ${bazel_compile_flags:-} =~ ${regex} ]]; then
        extra_db_contrib_args="${extra_db_contrib_args} --platform=${BASH_REMATCH[1]}"
    fi

    download_dir="./tmp_db_contrib_tool_download_dir"
    rm -rf "${download_dir}"

    invocation=""
    if [[ "${task_name:-}" == "archive_dist_test" ]]; then
        file_name="dist-test-stripped.${ext}"
        invocation="db-contrib-tool setup-repro-env ${build_patch_id} --variant=${compile_variant} --extractDownloads=False --binariesName=${file_name} --installDir=${download_dir} ${extra_db_contrib_args}"
    fi
    if [[ "${task_name:-}" == "archive_dist_test_debug" ]]; then
        file_name="dist-test-debug.${ext}"
        invocation="db-contrib-tool setup-repro-env ${build_patch_id} --variant=${compile_variant} --extractDownloads=False --debugsymbolsName=${file_name} --installDir=${download_dir} --skipBinaries --downloadSymbols ${extra_db_contrib_args}"
    fi

    if [[ -n "${invocation}" ]]; then
        setup_db_contrib_tool
        echo "db-contrib-tool invocation: ${invocation}"
        if ! eval ${invocation}; then
            echo "Could not retrieve files with db-contrib-tool"
            exit 1
        fi
        file_location=$(find "${download_dir}" -name "${file_name}")
        echo "Downloaded: ${file_location}"
        mkdir -p bazel-bin
        mv "${file_location}" "bazel-bin/${file_name}"
        echo "Moved ${file_name} to the correct location"
        echo "Skipping ${task_name} compile"
        exit 0
    fi

    echo "Could not skip ${task_name} compile, compiling as normal"
fi

# --build-mongot is a compile flag used by the evergreen build variants that run end-to-end search
# suites, as it downloads the necessary mongot binary.
if [[ "${build_mongot:-}" == "true" ]]; then
    setup_db_contrib_tool
    use_db_contrib_tool_mongot
    bazel_args="${bazel_args:-} --include_mongot=True"
fi

# This is hacky way to pass off build time from archive_dist_test to archive_dist_test_debug
# We create stripped binaries in archive_dist_test to avoid link time due to debug symbols
# We then create the symbols normally in archive_dist_test_debug. We have to force the
# build-id for debugging as they will be different when -Wl,-S is passed in.
# The relinked binaries should still be hash identical when stripped with strip
if [[ "${skip_debug_link:-}" == "true" ]]; then
    export compile_variant="${compile_variant}"
    export version_id="${version_id}"
    if [[ "${task_name:-}" == "archive_dist_test" ]]; then
        task_compile_flags="${task_compile_flags:-} --simple_build_id=True --features=strip_debug --separate_debug=False"
        if [[ "${remote_link:-}" == "true" ]]; then
            ARCH=$(uname -m)
            # Remote linking is currently only supported on arm
            if [[ "$ARCH" == "arm64" || "$ARCH" == "aarch64" ]]; then
                task_compile_flags="${task_compile_flags} --remote_download_outputs=toplevel --config=remote_link"
            fi
        fi
    fi
    if [[ "${task_name:-}" == "archive_dist_test_debug" ]]; then
        task_compile_flags="${task_compile_flags:-} --simple_build_id=True"
    fi
fi

# Use `eval` to force evaluation of the environment variables in the echo statement:
eval echo "Execution environment: Targets: ${targets}"

BAZEL_BINARY="$(bazel_evergreen_shutils::bazel_get_binary_path)"

# Compose LOCAL_ARG and release flags
LOCAL_ARG="$(bazel_evergreen_shutils::compute_local_arg build)"

# If we are doing a patch build or we are building a non-push
# build on the waterfall, then we don't need the --release
# flag. Otherwise, this is potentially a build that "leaves
# the building", so we do want that flag.
LOCAL_ARG="$(bazel_evergreen_shutils::maybe_release_flag "$LOCAL_ARG")"

# Ensure server is up and print PID
bazel_evergreen_shutils::ensure_server_and_print_pid "$BAZEL_BINARY"

# Build flags line
ALL_FLAGS="--verbose_failures ${LOCAL_ARG} ${bazel_args:-} ${bazel_compile_flags:-} ${task_compile_flags:-} --define=MONGO_VERSION=${version} ${patch_compile_flags:-}"
echo "${ALL_FLAGS}" >.bazel_build_flags

# Save the entire bazel build invocation to attach to the task for re-running locally
echo "bazel build ${ALL_FLAGS} ${targets}" >.bazel_build_invocation

set +o errexit

export RETRY_ON_FAIL=1
bazel_evergreen_shutils::retry_bazel_cmd 3 "$BAZEL_BINARY" \
    build ${ALL_FLAGS} ${targets}
RET=$?

bazel_evergreen_shutils::write_last_engflow_link

set -o errexit

if [[ "$RET" -eq 124 ]]; then
    echo "Bazel build timed out after ${build_timeout_seconds:-<unspecified>} seconds."
elif [[ "$RET" != "0" ]]; then
    echo "Bazel build failed after retries."
fi

: "${RET:=1}"
exit "${RET}"
