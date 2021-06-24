DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit

activate_venv

PATH="$PATH:/data/multiversion"
$python buildscripts/evergreen_generate_resmoke_tasks.py --expansion-file ../expansions.yml --verbose
$python buildscripts/evergreen_gen_multiversion_tests.py generate-exclude-tags
