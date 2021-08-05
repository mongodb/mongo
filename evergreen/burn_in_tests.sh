DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose
activate_venv

# Multiversion exclusions can be used when selecting tests.
PATH="$PATH:/data/multiversion"
$python buildscripts/evergreen_gen_multiversion_tests.py generate-exclude-tags --output=multiversion_exclude_tags.yml

# Capture a list of new and modified tests. The expansion macro burn_in_tests_build_variant
# is used to for finding the associated tasks from a different build varaint than the
# burn_in_tests_gen task executes on.
build_variant_opts="--build-variant=${build_variant}"
if [ -n "${burn_in_tests_build_variant}" ]; then
  build_variant_opts="--build-variant=${burn_in_tests_build_variant} --run-build-variant=${build_variant}"
fi
burn_in_args="$burn_in_args --repeat-tests-min=2 --repeat-tests-max=1000 --repeat-tests-secs=600"
# Evergreen executable is in $HOME.
PATH="$PATH:$HOME" eval $python buildscripts/evergreen_burn_in_tests.py --project=${project} $build_variant_opts --distro=${distro_id} --generate-tasks-file=burn_in_tests_gen.json --task_id ${task_id} $burn_in_args --verbose
