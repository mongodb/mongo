DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o errexit
set -o verbose
if [ "${use_wt_develop}" = "true" ]; then
    echo "Using the wtdevelop module instead..."
    cd src/third_party
    for wtdir in dist examples ext lang src test tools; do
        rm -rf wiredtiger/$wtdir
        mv wtdevelop/$wtdir wiredtiger/
    done
fi
