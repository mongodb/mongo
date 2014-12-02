#!/bin/sh
set -o errexit
tags=""
if [ ! -z "$1" ]
  then
  	tags="$@"
fi

. ./set_gopath.sh
mkdir -p bin
export GOBIN=bin

for i in bsondump mongostat mongofiles mongoexport mongoimport mongorestore mongodump mongotop mongooplog; do
	echo "Building ${i}..."
	go install -tags "$tags" "$i/main/$i.go"
done
