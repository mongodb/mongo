DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

cd src
source buildscripts/clang_tidy.sh ${clang_tidy_toolchain}
