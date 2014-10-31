#!/bin/sh
set -o errexit

. ./set_gopath.sh
mkdir bin || true
export GOBIN=bin

for i in bsondump mongostat mongofiles mongoexport mongoimport mongorestore mongodump mongotop ; do
	echo "Building ${i}..."
	go install "$i/main/$i.go"
done
