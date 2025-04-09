# Usage:
#   bazel_test [arguments]
#
# Required environment variables:
# * ${target} - Test target
# * ${args} - Extra command line args to pass to "bazel test"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv

# Use `eval` to force evaluation of the environment variables in the echo statement:
eval echo "Execution environment: Args: ${args} Target: ${target}"

source ./evergreen/bazel_utility_functions.sh
BAZEL_BINARY=$(bazel_get_binary_path)

# Print command being run to file that can be uploaded
echo "python buildscripts/install_bazel.py" > bazel-invocation.txt
echo "bazel test ${args} ${target}" >> bazel-invocation.txt

set +e

# use eval since some flags have quotes-in-quotes that are otherwise misinterpreted
eval $BAZEL_BINARY test ${args} ${target}
ret=$?

set -e

# For a target //path:test, the undeclared test outputs are in
# bazel-testlogs/path/test/test.outputs/outputs.zip
outputs=bazel-testlogs/$(sed "s|//||;s|:|/|" <<< ${target})/test.outputs/outputs.zip
if [ -f $outputs ]; then
  unzip $outputs -d ../
fi

exit $ret
