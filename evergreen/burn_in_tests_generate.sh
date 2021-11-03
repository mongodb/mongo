DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit

activate_venv

# Multiversion exclusions can be used when selecting tests.
PATH="$PATH:/data/multiversion"
$python buildscripts/resmoke.py generate-multiversion-exclude-tags --oldBinVersion=last_continuous --excludeTagsFilePath=multiversion_exclude_tags_last_continuous.yml
$python buildscripts/resmoke.py generate-multiversion-exclude-tags --oldBinVersion=last_lts --excludeTagsFilePath=multiversion_exclude_tags_last_lts.yml

PATH=$PATH:$HOME $python buildscripts/burn_in_tags.py --expansion-file ../expansions.yml
