DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src/jstestfuzz

set -o errexit
set -o verbose

add_nodejs_to_path

eval npm run ${npm_command} -- ${jstestfuzz_vars} --branch ${branch_name}
