#/bin/sh

set -e

rm -rf specifications

git clone git@github.com:mongodb/specifications

go generate ../