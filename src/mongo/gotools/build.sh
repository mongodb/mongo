#!/bin/sh
set -o errexit
tags=""
if [ ! -z "$1" ]
  then
  	tags="$@"
fi

# make sure we're in the directory where the script lives
SCRIPT_DIR="$(cd "$(dirname ${BASH_SOURCE[0]})" && pwd)"
cd $SCRIPT_DIR

sed -i.bak -e "s/built-without-version-string/$(git describe)/" \
           -e "s/built-without-git-spec/$(git rev-parse HEAD)/" \
           common/options/options.go

# remove stale packages
rm -rf vendor/pkg

. ./set_gopath.sh
mkdir -p bin

ec=0
for i in bsondump mongostat mongofiles mongoexport mongoimport mongorestore mongodump mongotop mongoreplay; do
        echo "Building ${i}..."
        go build -o "bin/$i" -tags "$tags" "$i/main/$i.go" || { echo "Error building $i"; ec=1; break; }
        ./bin/$i --version | head -1
done

if [ -t /dev/stdin ]; then
    stty sane
fi

mv -f common/options/options.go.bak common/options/options.go
exit $ec
