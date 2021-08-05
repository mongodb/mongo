DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

add_nodejs_to_path

git clone git@github.com:10gen/jstestfuzz.git

pushd jstestfuzz
npm install
npm run prepare
popd
