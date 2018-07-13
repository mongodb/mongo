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

VersionStr="$(git describe)"
Gitspec="$(git rev-parse HEAD)"
importpath="github.com/mongodb/mongo-tools/common/options"
ldflags="-X ${importpath}.VersionStr=${VersionStr} -X ${importpath}.Gitspec=${Gitspec}"

# remove stale packages
rm -rf vendor/pkg

. ./set_gopath.sh
mkdir -p bin

ec=0
for i in bsondump mongostat mongofiles mongoexport mongoimport mongorestore mongodump mongotop mongoreplay; do
        echo "Building ${i}..."
        go build -o "bin/$i" -ldflags "$ldflags" -tags "$tags" "$i/main/$i.go" || { echo "Error building $i"; ec=1; break; }
        ./bin/$i --version | head -1
done

if [ -t /dev/stdin ]; then
    stty sane
fi

exit $ec
