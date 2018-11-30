#!/bin/bash
set -o errexit
tags=""
if [ ! -z "$1" ]
  then
  	tags="$@"
fi

# make sure we're in the directory where the script lives
SCRIPT_DIR="$(cd "$(dirname ${BASH_SOURCE[0]})" && pwd)"
cd $SCRIPT_DIR

. ./set_goenv.sh
set_goenv || exit

# remove stale packages
rm -rf vendor/pkg

# build binaries for any tests that expect them for blackbox testing
./build.sh $tags

ec=0

# common can't recurse because we need different flags for different packages

# Test common/db with test type flags
for i in common/db; do
        echo "Testing ${i}..."
        (go test -ldflags "$(print_ldflags)" -tags "$(print_tags $tags)" ./$i -test.types=unit,integration) || { echo "Error testing $i"; ec=1; }
done

for i in common/db mongostat mongofiles mongoexport mongoimport mongorestore mongodump mongotop; do
        echo "Testing ${i}..."
        (cd $i && go test -ldflags "$(print_ldflags)" -tags "$(print_tags $tags)" ./... -test.types=unit,integration) || { echo "Error testing $i"; ec=1; }
done

# These don't support the test.types flag
common_with_test=$(find common -iname '*_test.go' | xargs -I % dirname % | sort -u | grep -v 'common/db')
for i in bsondump mongoreplay $common_with_test; do
        echo "Testing ${i}..."
        (cd $i && go test -ldflags "$(print_ldflags)" -tags "$(print_tags $tags)" . ) || { echo "Error testing $i"; ec=1; }
done

if [ -t /dev/stdin ]; then
    stty sane
fi

exit $ec
