#!/bin/bash
# This script downloads the sources of SpiderMonkey

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=spidermonkey

VERSION="140.7.0esr"
LIB_GIT_BRANCH=spidermonkey-esr140.7-cpp-only
LIB_GIT_REVISION=c822e4d076b36d878d95ad26d0f5ee7942dc39c4
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
