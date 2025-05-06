#!/usr/bin/env bash
#
# Script to build libtcmalloc.so in the current WiredTiger git
# repository.
#
# There is no justification for building tcmalloc rather you should
# grab a pre-built tcmalloc from a suitable spawn host.
#
# To use this script it is necessary to retrieve the url of patched
# source code archive, and the top level directory within that
# archive. That can be found in the CI build script:
#
# test/evergreen/tcmalloc_install_or_build.sh
#
# And is expected to look something like:
#
# https://github.com/mongodb-forks/tcmalloc/archive/refs/tags/mongo-SERVER-85737.tar.gz

set -euf -o pipefail

die() { echo "$@"; exit 1; }

# Require the MongoDB toolchain.
MDBTOOLCHAIN_PATH=/opt/mongodbtoolchain/v5/bin

[[ $# -ne 2 ]] && die "Usage: $0 url srcdir"
url=$1
srcdir=$2

# Check requirements.

HERE=$(git rev-parse --show-toplevel) || die "FATAL Run in root of git workspace"
[[ -f ${HERE}/CMakeLists.txt ]] || die "Does not look like a WT workspace - no CMakeLists.txt"
[[ -d ${HERE}/TCMALLOC_LIB ]] && die "TCMALLOC_LIB already present"

which bazel > /dev/null 2>&1 || die "FATAL Build tool bazel not found in path"
[[ -d $MDBTOOLCHAIN_PATH ]] || die "FATAL Requires mongodb toolchain."

tarball=$(basename $url)

# Retrieve and prepare for building.

curl --retry 5 -L $url -sS --max-time 120 --fail --output ${tarball}
tar zxf ${tarball}

# Create Bazel BUILD file in the top of the source directory tree to
# build the WiredTiger shared object.
cat << EOF > ${srcdir}/BUILD
package(default_visibility = ["//visibility:private"])

cc_shared_library(
    name = "libtcmalloc",
    deps = [
     "//tcmalloc:tcmalloc",
    ],
    shared_lib_name = "libtcmalloc.so",
    visibility = ["//visibility:public"],
)
EOF

# Build and copy to the expected location.

(cd $srcdir
 PATH=$MDBTOOLCHAIN_PATH:$PATH bazel build libtcmalloc)

mkdir TCMALLOC_LIB
cp $srcdir/bazel-bin/libtcmalloc.so TCMALLOC_LIB

exit 0
