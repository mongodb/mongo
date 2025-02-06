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

# Set the base URL for the bazelisk binaries to download from our s3 bucket
export BAZELISK_BASE_URL=https://mdb-build-public.s3.amazonaws.com/bazel-binaries

BAZEL_BINARY=$(bazel_get_binary_path)
if is_s390x_or_ppc64le; then
  # Set the JAVA_HOME directories for ppc64le and s390x since their bazel binaries are not compiled with a built-in JDK.
  export JAVA_HOME="/usr/lib/jvm/java-21-openjdk"
fi

for i in {1..5}; do
  eval $BAZEL_BINARY build --verbose_failures $LOCAL_ARG ${args} ${targets} && RET=0 && break || RET=$? && sleep 1
  echo "Bazel failed to execute, retrying..."
done

exit $RET
