#!/bin/sh

# how we got the last firefox sources

VERSION=45.0.2esr
TARBALL=firefox-$VERSION.source.tar.xz
if [ ! -f $TARBALL ]; then
    wget "https://ftp.mozilla.org/pub/mozilla.org/firefox/releases/$VERSION/source/$TARBALL"
fi

xzcat $TARBALL | tar -xf-

mv firefox-$VERSION mozilla-release
