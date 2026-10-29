DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

if [[ "${project:-}" != "mongodb-mongo-master" ]]; then
    echo "monitor_build_status must run from the mongodb-mongo-master project, but is running from '${project:-}'" >&2
    exit 1
fi

cd src
activate_venv

command_invocation="$python buildscripts/monitor_build_status/cli.py"
if [ "${is_patch}" != "true" ]; then
    command_invocation="$command_invocation --notify"
fi

echo "Verbatim monitor_build_status invocation: ${command_invocation}"
eval "${command_invocation}"
