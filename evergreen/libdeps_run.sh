DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv
GRAPH_FILE=$(find build -name "libdeps.graphml")
python buildscripts/libdeps/gacli.py --graph-file $GRAPH_FILE > results.txt
gzip $GRAPH_FILE
mv $GRAPH_FILE.gz .
