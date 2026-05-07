# TODO (SERVER-94776): Expose a reusable bazel command processor

# Usage:
#   bazel_coverage [arguments]
#
# Required environment variables:
# * ${target} - Build target
# * ${args} - Extra command line args to pass to "bazel coverage"

# Needed for evergreen scripts that use evergreen expansions and utility methods.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"
. "$DIR/bazel_evergreen_shutils.sh"

cd src

set -o errexit
set -o verbose

# Use `eval` to force evaluation of the environment variables in the echo statement:
eval echo "Execution environment: Args: ${args} Target: ${target}"

BAZEL_BINARY="$(bazel_evergreen_shutils::bazel_get_binary_path)"
export BAZEL_BINARY

# Print command being run to file that can be uploaded
echo "python buildscripts/install_bazel.py" >bazel-invocation.txt

echo "  bazel coverage ${args} ${target}" >>bazel-invocation.txt
export MONGO_WRAPPER_OUTPUT_ALL=1
$BAZEL_BINARY coverage ${args} ${target}
