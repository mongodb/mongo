# Applies the patch file(s) listed in the custom_build_patch_files expansion.
#
# Custom builds keep their consumer-specific modifications to publicly-synced files in
# patch files (stored in locations that copybara does not sync to the public
# repository) and apply them here, right before building. See
# etc/evergreen_yml_components/custom_builds/README.md.

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
    git apply --stat "${patch_file}"
    git apply "${patch_file}"
done

git diff
