DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit

activate_venv
PATH="$PATH:/data/multiversion"

if [[ "${require_multiversion_setup}" = "true" && -n "${multiversion_exclude_tags_version}" ]]; then
  $python buildscripts/resmoke.py generate-multiversion-exclude-tags --oldBinVersion="${multiversion_exclude_tags_version}"
fi
