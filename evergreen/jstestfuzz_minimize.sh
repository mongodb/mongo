DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src/jstestfuzz

set -o errexit
set -o verbose

if [ -f "../minimizer-outputs.json" ]; then
  eval ./src/scripts/npm_run.sh ${npm_command} -- -j "../minimizer-outputs.json"
fi
