DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src
set -o verbose
set -o errexit

activate_venv
$python buildscripts/resmoke.py generate-fuzz-config --template ../jepsen/docker/control/mongodb/resources --output ../jepsen/docker/control/mongodb/resources --disableEncryptionFuzzing

echo "Print config fuzzer generated mongod.conf"
cat ../jepsen/docker/control/mongodb/resources/mongod.conf
echo "Print config fuzzer generated mongos.conf"
cat ../jepsen/docker/control/mongodb/resources/mongos.conf
