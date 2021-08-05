DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src/build

set -o errexit
set -o verbose

tar cfvz embedded-sdk-tests.tgz mongo-embedded-sdk-${version}
