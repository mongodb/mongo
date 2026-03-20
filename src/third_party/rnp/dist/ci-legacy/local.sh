#!/bin/bash
set -eux

: "${GPG_VERSION:=stable}"
: "${BUILD_MODE:=normal}"

rsync -a /usr/local/rnp /tmp
sudo -iu travis bash -x <<EOF
cd /tmp/rnp
env ${CXX:+CXX=$CXX} \
    ${CC:+CC=$CC} \
    GPG_VERSION=$GPG_VERSION \
    BUILD_MODE=$BUILD_MODE \
    ${RNP_TESTS:+RNP_TESTS=$RNP_TESTS} \
    ci/run.sh
EOF
