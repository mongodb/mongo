#/bin/sh

set -e

rm -rf specifications

git clone git@github.com:merizodb/specifications

go generate ../