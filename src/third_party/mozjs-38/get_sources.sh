#!/bin/sh

# how we got the last firefox sources

wget "ftp://ftp.mozilla.org/pub/mozilla.org/firefox/releases/38.0.1esr/source/firefox-38.0.1esr.source.tar.bz2"

tar -jxf firefox-38.0.1esr.source.tar.bz2

mv mozilla-esr38 mozilla-release
