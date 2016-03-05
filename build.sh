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

# remove stale packages
rm -rf vendor/pkg

. ./set_gopath.sh
mkdir -p bin

for i in bsondump mongostat mongofiles mongoexport mongoimport mongorestore mongodump mongotop mongooplog; do
	echo "Building ${i}..."
  	# Build the tool, using -ldflags to link in the current gitspec
        go build -o "bin/$i" -ldflags "-X github.com/mongodb/mongo-tools/common/options.Gitspec `git rev-parse HEAD` -X github.com/mongodb/mongo-tools/common/options.VersionStr $(git describe)" -tags "$tags" "$i/main/$i.go"
done
