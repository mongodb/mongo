DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src/jstestfuzz

set -eo pipefail
set -o verbose

# Run first with help which will do the install
# Then we can run it in parallel
./src/scripts/npm_run.sh --help
# Run parse-jsfiles on 50 files at a time with 32 processes in parallel.
# Skip javascript files in third_party directory
find "$PWD/../jstests" "$PWD/../src/mongo/db/modules/enterprise" -path "$PWD/../jstests/third_party" -prune -o -name "*.js" -print | xargs -P 32 -L 50 ./src/scripts/npm_run.sh parse-jsfiles --
