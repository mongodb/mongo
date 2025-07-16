DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit

cd src

# decompress debug symbols
mkdir debug_extract
tar -zxvf mongo-debugsymbols.tgz -C debug_extract

#  renames dist-test to usr for Antithesis
mv debug_extract/dist-test debug_extract/usr

# recompress debug symbols
tar -czvf mongo-debugsymbols.tgz -C debug_extract usr
