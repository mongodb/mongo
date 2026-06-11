#!/bin/bash
# This script downloads the sources of SpiderMonkey

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=spidermonkey

VERSION="140.11.0esr"
LIB_GIT_BRANCH=spidermonkey-esr140.11-cpp-only
LIB_GIT_REVISION=7aeaf7a44d420360bde75db8453636092c3e2f56
LIB_GIT_REPO=git@github.com:mongodb-forks/spidermonkey.git
# If a local spidermonkey repo exists, this can be faster than fetching from github:
# LIB_GIT_REPO=file:///home/ubuntu/spidermonkey/.git

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/mozjs

LIB_GIT_DIR=$(mktemp -d /tmp/import-spidermonkey.XXXXXX)
# Windows:
# LIB_GIT_DIR=$(mktemp -d /z/import-spidermonkey.XXXXXX)
trap "rm -rf $LIB_GIT_DIR" EXIT

git clone \
  --branch "$LIB_GIT_BRANCH" \
  --depth 1 \
  "$LIB_GIT_REPO" \
  "$LIB_GIT_DIR"
git -C "$LIB_GIT_DIR" checkout "$LIB_GIT_REVISION"

test -d $DEST_DIR/mozilla-release && rm -rf $DEST_DIR/mozilla-release
rm -rf $LIB_GIT_DIR/.git
mkdir -p $DEST_DIR/mozilla-release
(shopt -s dotglob; mv $LIB_GIT_DIR/* $DEST_DIR/mozilla-release)
