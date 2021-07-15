DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

activate_venv

set -o verbose
set -o errexit

# get the list of feature flags from the patched version
$python buildscripts/idl/gen_all_feature_flag_list.py --import-dir src --import-dir src/mongo/db/modules/enterprise/src
mv all_feature_flags.txt patch_all_feature_flags.txt

# get the list of feature flags from the base commit
git stash
if [ -d src/mongo/db/modules/enterprise ]; then
  pushd src/mongo/db/modules/enterprise
  git stash
  popd
fi

$python buildscripts/idl/gen_all_feature_flag_list.py --import-dir src --import-dir src/mongo/db/modules/enterprise/src
mv all_feature_flags.txt base_all_feature_flags.txt

set +o errexit
git stash pop
if [ -d src/mongo/db/modules/enterprise ]; then
  pushd src/mongo/db/modules/enterprise
  git stash pop
  popd
fi
set -o errexit

# print out the list of tests that previously had feature flag tag, that was
# enabled by default in the current patch, and currently don't have requires
# latests FCV tag
$python buildscripts/feature_flag_tags_check.py
