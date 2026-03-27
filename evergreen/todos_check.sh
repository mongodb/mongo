DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

activate_venv

set -o verbose
set -o errexit

if [ "${requester}" == "github_merge_queue" ]; then
    $python buildscripts/todo_check.py --merge-queue-branch "${branch_name}"
elif [ "${is_patch}" == "true" ]; then
    $python buildscripts/todo_check.py --patch-build ${version_id}
fi
