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

source ./evergreen/bazel_RBE_supported.sh

if bazel_rbe_supported; then
  LOCAL_ARG=""
else
  LOCAL_ARG="--config=local"
fi

# We only support explicitly limited arch for code coverage, so there
# are fewer conditionals here than elsewhere in more general utilities.
BAZEL_BINARY=bazel

# Print command being run to file that can be uploaded
echo "python buildscripts/install_bazel.py" > bazel-invocation.txt
echo "bazel coverage --keep_going --verbose_failures --config=dbg --linkstatic=False $LOCAL_ARG ${args} ${target}" >> bazel-invocation.txt

set +e

eval $BAZEL_BINARY coverage --keep_going --verbose_failures --config=dbg --linkstatic=False $LOCAL_ARG ${args} ${target}

# TODO(SERVER-97069): Uncomment when all unit tests are compatible with bazel sandboxing
# exit $RET
exit 0
