# TODO (SERVER-94776): Expose a reusable bazel command processor

# Usage:
#   bazel_coverage [arguments]
#
# Required environment variables:
# * ${target} - Build target
# * ${args} - Extra command line args to pass to "bazel coverage"

# Needed for evergreen scripts that use evergreen expansions and utility methods.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

# Use `eval` to force evaluation of the environment variables in the echo statement:
eval echo "Execution environment: Args: ${args} Target: ${target}"

# We only support explicitly limited arch for code coverage, so there
# are fewer conditionals here than elsewhere in more general utilities.
BAZEL_BINARY=bazel

# Print command being run to file that can be uploaded
echo "python buildscripts/install_bazel.py" > bazel-invocation.txt

# TODO(SERVER-99431): Remove when bazel test timeouts are under control
set +e

echo "       bazel coverage ${args} ${target}" >> bazel-invocation.txt
# use eval since some flags have quotes-in-quotes that are otherwise misintepreted
eval $BAZEL_BINARY coverage ${args} ${target}

exit 0
