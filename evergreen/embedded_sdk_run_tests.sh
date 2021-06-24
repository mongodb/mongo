DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o verbose
set -o errexit

activate_venv
"build/mongo-embedded-sdk-${version}/bin/mongo_embedded_test"
"build/mongo-embedded-sdk-${version}/bin/mongoc_embedded_test"
