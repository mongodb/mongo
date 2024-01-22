DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o verbose
set -o errexit

git diff --name-only origin/${branch_name}... --line-prefix="${workdir}/src/" --diff-filter=d >> modified_and_created_patch_files.txt
