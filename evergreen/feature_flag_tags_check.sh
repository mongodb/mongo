DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

activate_venv

set -o verbose
set -o errexit

enterprise_path="src/mongo/db/modules/enterprise"
diff_file_name="with_base_upstream.diff"

# get the list of feature flags from the patched version
$python buildscripts/idl/gen_all_feature_flag_list.py
mv all_feature_flags.txt patch_all_feature_flags.txt

# get the list of feature flags from the base commit
git --no-pager diff "$(git merge-base origin/${branch_name} HEAD)" --output="$diff_file_name" --binary
if [ -s "$diff_file_name" ]; then
  git apply -R "$diff_file_name"
fi

$python buildscripts/idl/gen_all_feature_flag_list.py
mv all_feature_flags.txt base_all_feature_flags.txt

# print out the list of tests that previously had feature flag tag, that was
# enabled by default in the current patch, and currently don't have requires
# latests FCV tag
$python buildscripts/feature_flag_tags_check.py --diff-file-name="$diff_file_name" --enterprise-path="$enterprise_path"
