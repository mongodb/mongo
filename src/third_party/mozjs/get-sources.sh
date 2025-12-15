#!/bin/bash
# This script downloads the sources of SpiderMonkey

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=spidermonkey

VERSION="140.3.0esr"
LIB_GIT_BRANCH=aveselova/spidermonkey-esr140.3-cpp-only
LIB_GIT_REVISION=54ce5c4f64002c110069eba7861399fbf4b24ecc
LIB_GIT_REPO=git@github.com:mongodb-forks/spidermonkey.git
# If a local spidermonkey repo exists, this is much faster than fetching from git:
# LIB_GIT_REPO=/home/ubuntu/spidermonkey/.git

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/mozjs

LIB_GIT_DIR=$(mktemp -d /tmp/import-spidermonkey.XXXXXX)
# Windows:
# LIB_GIT_DIR=$(mktemp -d /z/import-spidermonkey.XXXXXX)
trap "rm -rf $LIB_GIT_DIR" EXIT

git clone $LIB_GIT_REPO $LIB_GIT_DIR
git -C $LIB_GIT_DIR checkout $LIB_GIT_REVISION

test -d $DEST_DIR/mozilla-release && rm -rf $DEST_DIR/mozilla-release
rm -rf $LIB_GIT_DIR/.git
mkdir -p $DEST_DIR/mozilla-release
(shopt -s dotglob; mv $LIB_GIT_DIR/* $DEST_DIR/mozilla-release)
