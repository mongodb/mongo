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

for i in bsondump mongostat mongofiles mongoexport mongoimport mongorestore mongodump mongotop mongooplog mongoreplay; do
        echo "Building ${i}..."
        go build -o "bin/$i" -tags "$tags" "$i/main/$i.go"
        ./bin/$i --version
done

mv -f common/options/options.go.bak common/options/options.go
