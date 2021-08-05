DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -eo pipefail
set -o verbose

add_nodejs_to_path

# Run parse-jsfiles on 50 files at a time with 32 processes in parallel.
# Grep returns 1 if it fails to find a match.
(grep -v "\.tpl\.js$" modified_and_created_patch_files.txt | grep "\.js$" || true) | xargs -P 32 -L 50 npm run --prefix jstestfuzz parse-jsfiles --
