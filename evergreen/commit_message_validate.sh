DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o verbose
set -o errexit
if [ "${is_commit_queue}" = "true" ]; then
  activate_venv
  $python buildscripts/validate_commit_message.py ${version_id}
fi
