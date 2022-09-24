DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o pipefail
set -o verbose

cd src
activate_venv
$python buildscripts/clang_tidy_condensed.py --clang-tidy-cfg ${clang_tidy_file} --clang-tidy-toolchain ${clang_tidy_toolchain}
exit $?
