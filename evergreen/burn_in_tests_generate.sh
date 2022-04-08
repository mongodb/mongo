DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv

# Evergreen executable is in $HOME.
PATH=$PATH:$HOME $python buildscripts/burn_in_tags.py --expansion-file ../expansions.yml --install-dir "${install_dir}"
