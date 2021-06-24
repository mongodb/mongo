DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

# Only run on master branch
if [ "${project}" == "mongodb-mongo-master" -a "${is_patch}" == "true" ]; then
  activate_venv
  PATH=$PATH:$HOME $python buildscripts/selected_tests.py --expansion-file ../expansions.yml --selected-tests-config .selected_tests.yml
fi
