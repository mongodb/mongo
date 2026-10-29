DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

activate_venv

set -o verbose
set -o errexit

enterprise_path="src/mongo/db/modules/enterprise"
diff_file_name="with_base_upstream.diff"

# get the list of feature flags from the patched version
$python buildscripts/idl/gen_all_feature_flag_list.py turned-on-by-default
mv all_feature_flags.txt patch_all_feature_flags.txt

# get the list of feature flags from the base commit
# --find-renames makes the fully disabled feature flag tags check treat renamed tests as
# renames of already existing tests rather than as new tests.
git --no-pager diff "$(git merge-base origin/${branch_name} HEAD)" --output="$diff_file_name" --binary --find-renames

if [ -s "$diff_file_name" ]; then
    # Either checker can temporarily reverse-apply the patch. Restore it on every exit path,
    # including an interrupted checker.
    restore_patched_tree() {
        if git apply --check "$diff_file_name"; then
            git apply "$diff_file_name" || echo "WARNING: could not restore the patched working tree" >&2
        fi
    }
    trap restore_patched_tree EXIT

    # fail if the patch/commit newly tags a jstest with a feature flag from
    # fully_disabled_feature_flags.yml, since such a test can never run on any variant
    $python buildscripts/fully_disabled_feature_flag_tags_check.py --diff-file-name="$diff_file_name" --enterprise-path="$enterprise_path"
fi

if [ -s "$diff_file_name" ]; then
    # Reverting the patch leaves the whole checkout at the base commit, which everything
    # running after this -- including the project-wide post block -- would otherwise execute
    # against.
    git apply -R "$diff_file_name"
fi

$python buildscripts/idl/gen_all_feature_flag_list.py turned-on-by-default
mv all_feature_flags.txt base_all_feature_flags.txt

# print out the list of tests that previously had feature flag tag, that was
# enabled by default in the current patch, and currently don't have requires
# latests FCV tag
$python buildscripts/feature_flag_tags_check.py --diff-file-name="$diff_file_name" --enterprise-path="$enterprise_path"
