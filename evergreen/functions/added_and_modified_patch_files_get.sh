DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o verbose
set -o errexit

git diff HEAD --name-only --line-prefix="${workdir}/src/" --diff-filter=d >> modified_and_created_patch_files.txt
if [ -d src/mongo/db/modules/enterprise ]; then
  pushd src/mongo/db/modules/enterprise
  git diff HEAD --name-only --line-prefix="${workdir}/src/src/mongo/db/modules/enterprise/" --diff-filter=d >> ~1/modified_and_created_patch_files.txt
  popd
fi
