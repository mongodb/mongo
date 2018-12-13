#! /bin/bash
#
# Copyright 2017 James E. King III
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
#      http://www.boost.org/LICENSE_1_0.txt)
#
# Bash script to run in travis to perform a Coverity Scan build
# To skip the coverity integration download (which is huge) if
# you already have it from a previous run, add --skipdownload
#

#
# Environment Variables
#
# COVERITY_SCAN_NOTIFICATION_EMAIL  - email address to notify
# COVERITY_SCAN_TOKEN               - the Coverity Scan token (should be secure)
# SELF                              - the boost libs directory name

set -ex

pushd /tmp
if [[ "$1" != "--skipdownload" ]]; then
  rm -rf coverity_tool.tgz cov-analysis*
  wget https://scan.coverity.com/download/linux64 --post-data "token=$COVERITY_SCAN_TOKEN&project=boostorg/$SELF" -O coverity_tool.tgz
  tar xzf coverity_tool.tgz
fi
COVBIN=$(echo $(pwd)/cov-analysis*/bin)
export PATH=$COVBIN:$PATH
popd

ci/build.sh clean
rm -rf cov-int/
cov-build --dir cov-int ci/build.sh
tar cJf cov-int.tar.xz cov-int/
curl --form token="$COVERITY_SCAN_TOKEN" \
     --form email="$COVERITY_SCAN_NOTIFICATION_EMAIL" \
     --form file=@cov-int.tar.xz \
     --form version="$(git describe --tags)" \
     --form description="boostorg/$SELF" \
     https://scan.coverity.com/builds?project="boostorg/$SELF"

