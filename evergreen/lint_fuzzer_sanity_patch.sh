DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

cd src

activate_venv
base_revision="$(git merge-base ${revision} HEAD)"
echo "Base patch revision: $base_revision"
REVISION=$base_revision $python evergreen/lint_fuzzer_sanity_patch.py
