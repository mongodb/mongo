DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src/jstestfuzz

set -eo pipefail
set -o verbose

# Run first with help which will do the install
# Then we can run it in parallel
./src/scripts/npm_run.sh --help
# Run parse-jsfiles on 50 files at a time with 32 processes in parallel.
# Grep returns 1 if it fails to find a match.
(grep -v "\.tpl\.js$" ../modified_and_created_patch_files.txt | grep "\.js$" || true) | xargs -P 32 -L 50 ./src/scripts/npm_run.sh parse-jsfiles --
