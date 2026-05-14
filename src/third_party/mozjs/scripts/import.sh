#!/bin/bash
# This script downloads the sources of SpiderMonkey

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=spidermonkey

VERSION="140.9.0esr"
LIB_GIT_BRANCH=spidermonkey-esr140.9-cpp-only
LIB_GIT_REVISION=dd4a007275927572d0b10857d3f11b28550d0095
LIB_GIT_REPO=git@github.com:mongodb-forks/spidermonkey.git
# If a local spidermonkey repo exists, this can be faster than fetching from github:
# LIB_GIT_REPO=file:///home/ubuntu/spidermonkey/.git

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/mozjs

LIB_GIT_DIR=$(mktemp -d /tmp/import-spidermonkey.XXXXXX)
# Windows:
# LIB_GIT_DIR=$(mktemp -d /z/import-spidermonkey.XXXXXX)
trap "rm -rf $LIB_GIT_DIR" EXIT

git clone \
  --revision "$LIB_GIT_REVISION" \
  --depth 1 \
  "$LIB_GIT_REPO" \
  "$LIB_GIT_DIR"

test -d $DEST_DIR/mozilla-release && rm -rf $DEST_DIR/mozilla-release
rm -rf $LIB_GIT_DIR/.git
mkdir -p $DEST_DIR/mozilla-release
(shopt -s dotglob; mv $LIB_GIT_DIR/* $DEST_DIR/mozilla-release)
