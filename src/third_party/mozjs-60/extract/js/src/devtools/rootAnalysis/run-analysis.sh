#!/bin/sh

SRCDIR=$(cd $(dirname $0)/../../../..; pwd)
GECKO_DIR=$SRCDIR $SRCDIR/taskcluster/scripts/builder/build-haz-linux.sh $(pwd) "$@"
