# Usage:
#   bazel_compile [arguments]
#
# Required environment variables:
# * ${targets} - List of build targets
#
# Optional environment variable(s):
# * ${args} - List of additional Bazel arguments (e.g.: "--config=clang-tidy")

# Needed for evergreen scripts that use evergreen expansions and utility methods.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv

# if build_patch_id is passed, try to download binaries from specified
# evergreen patch.
build_patch_id="${build_patch_id:-${reuse_compile_from}}"
if [ -n "${build_patch_id}" ]; then
  echo "build_patch_id detected, trying to skip task"
  if [ "${task_name}" = "compile_dist_test" ] || [ "${task_name}" = "compile_dist_test_half" ]; then
    echo "Skipping ${task_name} compile without downloading any files"
    exit 0
  fi

  # On windows we change the extension to zip
  if [ -z "${ext}" ]; then
    ext="tgz"
  fi

  extra_db_contrib_args=""

  # get the platform of the dist archive. This is needed if
  # db-contrib-tool cannot autodetect the platform of the ec2 instance.
  regex='MONGO_DISTMOD=([a-z0-9]*)'
  if [[ ${compile_flags} =~ ${regex} ]]; then
    extra_db_contrib_args="${extra_db_contrib_args} --platform=${BASH_REMATCH[1]}"
  fi

  download_dir="./tmp_db_contrib_tool_download_dir"
  rm -rf ${download_dir}

  if [ "${task_name}" = "compile_dist_test" ]; then
    file_name="mongodb-binaries.${ext}"
    invocation="db-contrib-tool setup-repro-env ${build_patch_id} \
      --variant=${compile_variant} --extractDownloads=False \
      --binariesName=${file_name} --installDir=${download_dir} ${extra_db_contrib_args}"
  fi

  if [ "${task_name}" = "archive_dist_test_debug" ]; then
    file_name="mongo-debugsymbols.${ext}"
    invocation="db-contrib-tool setup-repro-env ${build_patch_id} \
      --variant=${compile_variant} --extractDownloads=False \
      --debugsymbolsName=${file_name} --installDir=${download_dir} \
      --skipBinaries --downloadSymbols ${extra_db_contrib_args}"
  fi

  if [ -n "${invocation}" ]; then
    setup_db_contrib_tool

    echo "db-contrib-tool invocation: ${invocation}"
    eval ${invocation}
    if [ $? -ne 0 ]; then
      echo "Could not retrieve files with db-contrib-tool"
      exit 1
    fi
    file_location=$(find "${download_dir}" -name "${file_name}")
    echo "Downloaded: ${file_location}"
    mv "${file_location}" "${file_name}"
    echo "Moved ${file_name} to the correct location"
    echo "Skipping ${task_name} compile"
    exit 0
  fi

  echo "Could not skip ${task_name} compile, compiling as normal"
fi

# --build-mongot is a compile flag used by the evergreen build variants that run end-to-end search
# suites, as it downloads the necessary mongot binary.
if [ "${build_mongot}" = "true" ]; then
  setup_db_contrib_tool
  use_db_contrib_tool_mongot
fi

set -o pipefail

# Use `eval` to force evaluation of the environment variables in the echo statement:
eval echo "Execution environment: Targets: ${targets}"

source ./evergreen/bazel_RBE_supported.sh
source ./evergreen/bazel_utility_functions.sh

if bazel_rbe_supported; then
  LOCAL_ARG=""
else
  LOCAL_ARG="--config=local"
fi

if [[ "${evergreen_remote_exec}" != "on" ]]; then
  LOCAL_ARG="$LOCAL_ARG --jobs=auto"
fi

BAZEL_BINARY=$(bazel_get_binary_path)

# Timeout is set here to avoid the build hanging indefinitely, still allowing
# for retries.
TIMEOUT_CMD=""
if [ -n "${build_timeout_seconds}" ]; then
  TIMEOUT_CMD="timeout ${build_timeout_seconds}"
fi

for i in {1..5}; do
  eval ${TIMEOUT_CMD} $BAZEL_BINARY build --verbose_failures $LOCAL_ARG ${args} ${targets} && RET=0 && break || RET=$? && sleep 1
  if [ $RET -eq 124 ]; then
    echo "Bazel timed out after ${build_timeout_seconds} seconds, retrying..."
  else
    echo "Bazel failed to execute, retrying..."
  fi
done

exit $RET
