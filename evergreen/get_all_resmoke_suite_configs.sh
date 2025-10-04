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

source ./evergreen/bazel_utility_functions.sh
BAZEL_BINARY=$(bazel_get_binary_path)

# Queries all resmoke_config targets: kind(resmoke_config, //...)
# and outputs YAML key-value pair created by the starlark expression for each target.
#   str(target.label).replace('@@','') -> the target name, like //buildscripts/resmokeconfig:core_config
#   f.path for f in target.files.to_list() -> the path to the config file, like bazel-out/k8-fastbuild/bin/buildscripts/resmokeconfig/core.yml
${BAZEL_BINARY} cquery ${bazel_args} ${bazel_compile_flags} ${task_compile_flags} \
    --define=MONGO_VERSION=${version} ${patch_compile_flags} "kind(resmoke_config, //...)" \
    --output=starlark --starlark:expr "': '.join([str(target.label).replace('@@','')] + [f.path for f in target.files.to_list()])" >resmoke_suite_configs.yml
