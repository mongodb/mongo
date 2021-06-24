DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose
activate_venv

$python buildscripts/idl/check_versioned_api_commands_have_idl_definitions.py -v --include src --include src/mongo/db/modules/enterprise/src --installDir dist-test/bin 1
$python buildscripts/idl/checkout_idl_files_from_past_releases.py -v idls
find idls -maxdepth 1 -mindepth 1 -type d | while read dir; do
  echo "Performing idl check compatibility with release: $dir:"
  $python buildscripts/idl/idl_check_compatibility.py -v --include src --include src/mongo/db/modules/enterprise/src "$dir/src" src
done
