DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv
GRAPH_FILE=$(find build -name "libdeps.graphml")
python buildscripts/libdeps/analyzer_unittests.py
python buildscripts/libdeps/gacli.py --graph-file $GRAPH_FILE > results.txt
python buildscripts/libdeps/gacli.py --graph-file $GRAPH_FILE --lint= --bazel-order > bazel_order.txt
gzip $GRAPH_FILE
mv $GRAPH_FILE.gz .
targets_converted=$(grep "Targets Converted:" bazel_order.txt | cut -d":" -f 2)
if ((targets_converted < "50")); then exit 1; fi
