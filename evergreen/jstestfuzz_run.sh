DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -evo pipefail

cd src/jstestfuzz

eval ./src/scripts/npm_run.sh ${npm_command} -- ${jstestfuzz_vars} --branch ${branch_name}
