# Warms the remote cache with the instrumented artifacts shared by every coverage task.
# Downstream `bazel coverage` tasks (the //src/mongo/... unittests and the per-suite
# //jstests/suites/... integration tasks) then get compile cache hits instead of each host
# re-compiling the instrumented server from cold. Link actions are still local (CppLink is
# tagged +no-remote-cache in .bazelrc), but compilation dominates, so this removes most of
# the redundant work when the integration task group fans out (max_hosts: -1).
#
# Usage:
#   bazel_coverage_compile [arguments]
#
# Required environment variables:
# * ${target} - Build target(s)
# * ${args} - Extra command line args to pass to "bazel build"

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

echo "  bazel build --config=coverage_compile ${args} ${target}" >>bazel-invocation.txt
export MONGO_WRAPPER_OUTPUT_ALL=1
$BAZEL_BINARY build --config=coverage_compile ${args} ${target}
