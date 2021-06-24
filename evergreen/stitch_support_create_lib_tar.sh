DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src/build

set -o errexit
set -o verbose

tar cfvz stitch-support.tgz stitch-support-lib-${version}
