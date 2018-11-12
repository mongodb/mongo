#!/bin/bash
# This script downloads and imports boost via the boost bcp utility.
# It can be run on Linux or Mac OS X.
# Actual integration into the build system is not done by this script.
#
# Turn on strict error checking, like perl use 'strict'
set -euo pipefail
IFS=$'\n\t'

if [ "$#" -ne 0 ]; then
    echo "This script does not take any arguments"
    exit 1
fi

NAME=boost
VERSION=1.68.0
VERSION_UNDERSCORE=$(echo $VERSION | tr . _)
SRC_ROOT=$(mktemp -d /tmp/boost.XXXXXX)
trap "rm -rf $SRC_ROOT" EXIT
TARBALL=${NAME}_${VERSION_UNDERSCORE}.tar.gz
SRC=${SRC_ROOT}/${NAME}_${VERSION_UNDERSCORE}
DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME-$VERSION

cd $SRC_ROOT

if [ ! -f $TARBALL ]; then
    wget http://downloads.sourceforge.net/project/boost/boost/$VERSION/$TARBALL
fi

if [ ! -d $SRC ]; then
    tar -zxf $TARBALL
fi

# Build the bcp tool
# The bcp tool is a boost specific tool that allows importing a subset of boost
# The downside is that it copies a lot of unnecessary stuff in libs
# and does not understand #ifdefs
#
cd $SRC

./bootstrap.sh

./b2 tools/bcp

test -d $DEST_DIR || mkdir $DEST_DIR
$SRC/dist/bin/bcp --boost=$SRC/ algorithm align array asio bind iostreams config container date_time filesystem function integer intrusive multi_index noncopyable optional program_options random smart_ptr static_assert unordered utility $DEST_DIR

# Trim files
cd $DEST_DIR

rm -f Jamroot boost.png
rm -rf doc

# Trim misc directories from libs that bcp pulled in
find libs -type d -name test -print0 | xargs -0 rm -rf
find libs -type d -name doc -print0 | xargs -0 rm -rf
find libs -type d -name build -print0 | xargs -0 rm -rf
find libs -type d -name examples -print0 | xargs -0 rm -rf
find libs -type d -name example -print0 | xargs -0 rm -rf
find libs -type d -name meta -print0 | xargs -0 rm -rf
find libs -type d -name tutorial -print0 | xargs -0 rm -rf
find libs -type d -name performance -print0 | xargs -0 rm -rf
find libs -type d -name bench -print0 | xargs -0 rm -rf
find libs -type d -name perf -print0 | xargs -0 rm -rf
find libs -type d -name proj -print0 | xargs -0 rm -rf
find libs -type d -name xmldoc -print0 | xargs -0 rm -rf
find libs -type d -name tools -print0 | xargs -0 rm -rf
find libs -type d -name extra -print0 | xargs -0 rm -rf
find libs -type d -name bug -print0 | xargs -0 rm -rf

find libs -name "*.html" -print0 | xargs -0 rm -f
find libs -name "*.htm" -print0 | xargs -0 rm -f
find libs -name "*.zip" -print0 | xargs -0 rm -f
find libs -name "*.gif" -print0 | xargs -0 rm -f

# Full of unneeded code
rm -rf libs/algorithm
rm -rf libs/config
rm -rf libs/static_assert

# Trim the include directory for the stuff bcp dragged in and we do not need
# since they are 1+ MB each
rm -f boost/typeof/vector100.hpp
rm -f boost/typeof/vector150.hpp
rm -f boost/typeof/vector200.hpp

# Remove compat files for compilers we do not support
find boost -type d -name dmc -print0 | xargs -0 rm -rf
find boost -type d -name "bcc*" -print0 | xargs -0 rm -rf
find boost -type d -name mwcw -print0 | xargs -0 rm -rf
find boost -type d -name msvc60 -print0 | xargs -0 rm -rf
find boost -type d -name msvc70 -print0 | xargs -0 rm -rf

find . -type d -empty -print0 | xargs -0 rmdir
