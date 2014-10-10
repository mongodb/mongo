#!/bin/bash

# exit on error 
set -e

# cache the working dir, to return to later
workingdir=`pwd`

TOOLS_PKG='github.com/mongodb/mongo-tools'

# make sure the working directory is the root of
# the repo
SCRIPT_DIR="$(cd "$(dirname ${BASH_SOURCE[0]})" && pwd)"
cd $SCRIPT_DIR

SOURCE_GOPATH=`pwd`.gopath
VENDOR_GOPATH=`pwd`/vendor
if [ "Windows_NT" = "$OS" ]; then
    SOURCE_GOPATH=$(cygpath -w $(SOURCE_GOPATH));
    VENDOR_GOPATH=$(cygpath -w $(VENDOR_GOPATH));
fi

# set up the $GOPATH to use the vendored dependencies as
# well as the source for the mongo tools
rm -rf .gopath/
mkdir -p .gopath/src/"$(dirname $TOOLS_PKG)"
ln -sf `pwd` .gopath/src/$TOOLS_PKG
export GOPATH=`pwd`/.gopath:`pwd`/vendor

# go back to the original working dir
cd $workingdir

# run the go command with the specified arguments
cmd="go ${@:1}"
eval "${cmd[@]}"
