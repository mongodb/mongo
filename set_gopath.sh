#!/bin/bash

# exit on error
set -e

# make sure the working directory is the root of
# the repo
setgopath() {
    local SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    cd $SCRIPT_DIR

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
