# Applies the patch file(s) listed in the custom_build_patch_files expansion.
#
# Custom builds keep their consumer-specific modifications to publicly-synced files in
# patch files (stored in locations that copybara does not sync to the public
# repository) and apply them here, right before building. See
# etc/evergreen_yml_components/custom_builds/README.md.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
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
  if git apply --check "${patch_file}" 2> /dev/null; then
    git apply --stat "${patch_file}"
    git apply "${patch_file}"
  elif git apply -R --check "${patch_file}" 2> /dev/null; then
    # The tasks of a task group share one workdir, so a patch applied by an earlier
    # task (for example archive_dist_test) is still present when a later task (for
    # example package) runs this script again. Treat that as a no-op.
    echo "Patch ${patch_file} is already applied (shared task group workdir), skipping"
  else
    echo "Patch ${patch_file} neither applies cleanly nor is fully applied" >&2
    exit 1
  fi
done

git diff
