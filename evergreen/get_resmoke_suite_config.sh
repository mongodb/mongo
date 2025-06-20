# Produces an Evergreen expansion file with the path to the resmoke config for ${suite}
#
# Usage:
#   bash get_resmoke_suite_config.sh
#
# Required environment variables:
# * ${suite} - Resmoke bazel target, like //buildscripts/resmokeconfig:core

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

source ./evergreen/bazel_utility_functions.sh
BAZEL_BINARY=$(bazel_get_binary_path)

echo "suite_config: $(${BAZEL_BINARY} cquery ${bazel_args} ${bazel_compile_flags} ${task_compile_flags} \
  --define=MONGO_VERSION=${version} ${patch_compile_flags} ${suite}_config --output files)" > suite_config_expansion.yml
