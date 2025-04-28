DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

cd src
activate_venv

command_invocation="$python buildscripts/monitor_build_status/cli.py"
if [ "${is_patch}" != "true" ]; then
  command_invocation="$command_invocation --notify"
fi

echo "Verbatim monitor_build_status invocation: ${command_invocation}"
eval "${command_invocation}"
