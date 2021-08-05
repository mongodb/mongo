DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv
$python buildscripts/evergreen_gen_powercycle_tasks.py ../expansions.yml ../powercycle_tasks.json
