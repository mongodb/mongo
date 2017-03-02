#!/bin/bash

TOOLS_PKG='github.com/bongodb/bongo-tools'

setgopath() {
	if [ "Windows_NT" != "$OS" ]; then
		SOURCE_GOPATH=`pwd`.gopath
		VENDOR_GOPATH=`pwd`/vendor

		# set up the $GOPATH to use the vendored dependencies as
		# well as the source for the bongo tools
		rm -rf .gopath/
		mkdir -p .gopath/src/"$(dirname "${TOOLS_PKG}")"
		ln -sf `pwd` .gopath/src/$TOOLS_PKG
		export GOPATH=`pwd`/.gopath:`pwd`/vendor
	else
		local SOURCE_GOPATH=`pwd`/.gopath
		local VENDOR_GOPATH=`pwd`/vendor
		SOURCE_GOPATH=$(cygpath -w $SOURCE_GOPATH);
		VENDOR_GOPATH=$(cygpath -w $VENDOR_GOPATH);

		# set up the $GOPATH to use the vendored dependencies as
		# well as the source for the bongo tools
		rm -rf .gopath/
		mkdir -p .gopath/src/"$TOOLS_PKG"
		cp -r `pwd`/bsondump .gopath/src/$TOOLS_PKG
		cp -r `pwd`/common .gopath/src/$TOOLS_PKG
		cp -r `pwd`/bongodump .gopath/src/$TOOLS_PKG
		cp -r `pwd`/bongoexport .gopath/src/$TOOLS_PKG
		cp -r `pwd`/bongofiles .gopath/src/$TOOLS_PKG
		cp -r `pwd`/bongoimport .gopath/src/$TOOLS_PKG
		cp -r `pwd`/bongooplog .gopath/src/$TOOLS_PKG
		cp -r `pwd`/bongorestore .gopath/src/$TOOLS_PKG
		cp -r `pwd`/bongostat .gopath/src/$TOOLS_PKG
		cp -r `pwd`/bongotop .gopath/src/$TOOLS_PKG
		cp -r `pwd`/vendor/src/github.com/* .gopath/src/github.com
		cp -r `pwd`/vendor/src/gopkg.in .gopath/src/
		export GOPATH="$SOURCE_GOPATH;$VENDOR_GOPATH"
	fi;
}

setgopath
