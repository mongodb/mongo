DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

activate_venv
$python -c 'import json; print(json.dumps([{
  "name": "*** How to use UndoDB Recordings instead of Core Dumps or Log Files ***",
  "link": "https://wiki.corp.mongodb.com/display/COREENG/Time+Travel+Debugging+in+MongoDB",
  "visibility": "public",
  "ignore_for_fetch": True
}]))' > undo_wiki_page_location.json
