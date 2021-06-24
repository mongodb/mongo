DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

activate_venv
$python -c 'import json; print(json.dumps([{
  "name": "Wiki: Running Tests from Evergreen Tasks Locally",
  "link": "https://github.com/mongodb/mongo/wiki/Running-Tests-from-Evergreen-Tasks-Locally",
  "visibility": "public",
  "ignore_for_fetch": True
}]))' > wiki_page_location.json
