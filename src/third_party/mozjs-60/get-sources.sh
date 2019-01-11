#!/bin/sh

# how we got the last firefox sources

VERSION=60.3.0
TARBALL=firefox-${VERSION}esr.source.tar.xz
if [ ! -f $TARBALL ]; then
    curl -O "https://ftp.mozilla.org/pub/mozilla.org/firefox/releases/${VERSION}esr/source/$TARBALL"
fi

xzcat $TARBALL | tar -xf-

mv firefox-$VERSION mozilla-release
