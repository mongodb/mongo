DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit

cd src
# decompress debug symbols
tar -zxvf mongo-debugsymbols.tgz
#  renames dist-test to usr for Antithesis
mv dist-test usr
# recompress debug symbols
tar -czvf mongo-debugsymbols.tgz usr
