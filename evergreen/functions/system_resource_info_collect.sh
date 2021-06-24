DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

activate_venv
$python buildscripts/collect_resource_info.py -o system_resource_info.json -i 5
