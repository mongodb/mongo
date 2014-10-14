#!/bin/bash

# exit on error
set -e

TOOLS_PKG='github.com/mongodb/mongo-tools'

setgopath() {
    local SOURCE_GOPATH=`pwd`.gopath
    local VENDOR_GOPATH=`pwd`/vendor
    if [ "Windows_NT" = "$OS" ]; then
        SOURCE_GOPATH=$(cygpath -w $(SOURCE_GOPATH));
        VENDOR_GOPATH=$(cygpath -w $(VENDOR_GOPATH));
    fi;

    # set up the $GOPATH to use the vendored dependencies as
    # well as the source for the mongo tools
    rm -rf .gopath/
    mkdir -p .gopath/src/"$(dirname "${TOOLS_PKG}")"
    ln -sf `pwd` .gopath/src/$TOOLS_PKG
    export GOPATH=`pwd`/.gopath:`pwd`/vendor
}

setgopath
