DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o verbose
set -o errexit
activate_venv
$python buildscripts/resmoke.py generate-fuzz-config --template evergreen/do_jepsen_setup --output jepsen-mongodb/resources

echo "Print config fuzzer generated mongod.conf"
cat jepsen-mongodb/resources/mongod.conf
