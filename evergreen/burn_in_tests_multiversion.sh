DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv
# Capture a list of new and modified tests. The expansion macro burn_in_tests_build_variant
# is used for finding the associated tasks from a different build variant than the
# burn_in_tests_multiversion_gen task executes on.
build_variant_opts="--build-variant=${build_variant}"
if [ -n "${burn_in_tests_build_variant}" ]; then
  build_variant_opts="--build-variant=${burn_in_tests_build_variant} --run-build-variant=${build_variant}"
fi

burn_in_args="$burn_in_args"
# Evergreen executable is in $HOME.
PATH="$PATH:$HOME" eval $python buildscripts/burn_in_tests_multiversion.py --task_id=${task_id} --project=${project} $build_variant_opts --distro=${distro_id} --generate-tasks-file=burn_in_tests_multiversion_gen.json $burn_in_args --verbose --revision=${revision} --build-id=${build_id}
PATH="$PATH:/data/multiversion"
$python buildscripts/evergreen_gen_multiversion_tests.py generate-exclude-tags
