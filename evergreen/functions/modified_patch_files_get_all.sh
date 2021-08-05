DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o verbose
set -o errexit

# For patch builds gather the modified patch files.
if [ "${is_patch}" = "true" ]; then
  # Get list of patched files
  git diff HEAD --name-only >> patch_files.txt
  if [ -d src/mongo/db/modules/enterprise ]; then
    pushd src/mongo/db/modules/enterprise
    # Update the patch_files.txt in the mongo repo.
    git diff HEAD --name-only >> ~1/patch_files.txt
    popd
  fi
fi
