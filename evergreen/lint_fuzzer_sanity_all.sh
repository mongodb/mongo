DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -eo pipefail
set -o verbose

add_nodejs_to_path

# Run parse-jsfiles on 50 files at a time with 32 processes in parallel.
find "$PWD/jstests" "$PWD/src/mongo/db/modules/enterprise" -name "*.js" -print | xargs -P 32 -L 50 npm run --prefix jstestfuzz parse-jsfiles --
