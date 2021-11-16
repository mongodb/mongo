DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

if [ -f "src/jstestfuzz/out/minimizer-outputs-minimizedtest.js" ]; then
  activate_venv
  $python -c 'import json; print(json.dumps([{
    "name": "Wiki: Running minimized Agg fuzzer and Query fuzzer tests locally",
    "link": "https://github.com/mongodb/mongo/wiki/Running-minimized-Agg-fuzzer-and-Query-fuzzer-tests-locally",
    "visibility": "public",
    "ignore_for_fetch": True
  }]))' > wiki_page_running_minimized_test_location.json
fi
