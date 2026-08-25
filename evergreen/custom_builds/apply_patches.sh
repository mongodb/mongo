# Applies the patch file(s) listed in the custom_build_patch_files expansion.
#
# Custom builds keep their consumer-specific modifications to publicly-synced files in
# patch files (stored in locations that copybara does not sync to the public
# repository) and apply them here, right before building. See
# etc/evergreen_yml_components/custom_builds/README.md.
#
# This hook runs in the shared "bazel compile" function, so every compile task of a
# custom build applies the patches in its own workdir. Task-group members share a
# workdir, so application is idempotent: a patch that is already applied is skipped
# rather than failing the second git apply.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o errexit
set -o verbose

if [ -z "${custom_build_patch_files:-}" ]; then
    echo "custom_build_patch_files is not set, no custom build patches to apply"
    exit 0
fi

for patch_file in ${custom_build_patch_files}; do
    echo "Applying custom build patch file: ${patch_file}"
    if git apply --check "${patch_file}" 2>/dev/null; then
        git apply --stat "${patch_file}"
        git apply "${patch_file}"
    elif git apply --reverse --check "${patch_file}" 2>/dev/null; then
        echo "Patch ${patch_file} is already applied in this workdir (shared task-group workdir); skipping"
    else
        echo "ERROR: ${patch_file} neither applies cleanly nor is already applied:" >&2
        git apply --check "${patch_file}" >&2
        exit 1
    fi
done

git diff
