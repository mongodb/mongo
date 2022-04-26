DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o verbose
set -o errexit
if [ "${is_commit_queue}" = "true" ]; then
  activate_venv
  $python -m pip --disable-pip-version-check install --upgrade cryptography==36.0.2 || exit 1
  $python buildscripts/validate_commit_message.py \
    --evg-config-file ./.evergreen.yml \
    ${version_id}
fi
