# Produces a YAML file mapping resmoke config targets to their output file paths.
#   example entry: //buildscripts/resmokeconfig:core_config: bazel-out/k8-fastbuild/bin/buildscripts/resmokeconfig/core.yml
#
# Usage:
#   bash get_all_resmoke_suite_configs.sh

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

source ./evergreen/bazel_evergreen_shutils.sh
BAZEL_BINARY=$(bazel_evergreen_shutils::bazel_get_binary_path)

# Queries all resmoke_config targets and outputs YAML key-value pairs mapping targets to their config files.
bazel_evergreen_shutils::query_resmoke_configs \
    "${BAZEL_BINARY}" \
    "${bazel_args} ${bazel_compile_flags} ${task_compile_flags} --define=MONGO_VERSION=${version} ${patch_compile_flags}" \
    "resmoke_suite_configs.yml"
