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
export GOBIN=bin

for i in bsondump mongostat mongofiles mongoexport mongoimport mongorestore mongodump mongotop mongooplog; do
	echo "Building ${i}..."
	go install -tags "$tags" "$i/main/$i.go"
done
