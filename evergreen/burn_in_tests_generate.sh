DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit

activate_venv

# Multiversion exclusions can be used when selecting tests.
PATH="$PATH:/data/multiversion"
$python buildscripts/evergreen_gen_multiversion_tests.py generate-exclude-tags --output=multiversion_exclude_tags.yml

PATH=$PATH:$HOME $python buildscripts/burn_in_tags.py --expansion-file ../expansions.yml
