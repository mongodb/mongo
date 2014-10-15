#!/bin/bash

TOOLS_PKG='github.com/mongodb/mongo-tools'

setgopath() {
    SOURCE_GOPATH=`pwd`.gopath
    VENDOR_GOPATH=`pwd`/vendor

    # set up the $GOPATH to use the vendored dependencies as
    # well as the source for the mongo tools
    rm -rf .gopath/
    mkdir -p .gopath/src/"$(dirname "${TOOLS_PKG}")"
    ln -sf `pwd` .gopath/src/$TOOLS_PKG
    export GOPATH=`pwd`/.gopath:`pwd`/vendor
}

setgopath
