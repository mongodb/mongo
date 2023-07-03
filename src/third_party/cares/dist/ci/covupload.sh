#!/bin/bash
set -e
if [ "$BUILD_TYPE" = "coverage" ]; then
    echo "CI_NAME=${CI_NAME}"
    echo "CI_BUILD_NUMBER=${CI_BUILD_NUMBER}"
    echo "CI_BUILD_URL=${CI_BUILD_URL}"
    echo "CI_BRANCH=${CI_BRANCH}"
    echo "CI_PULL_REQUEST=${CI_PULL_REQUEST}"
    PATH="${PATH}:/root/.local/bin:~/.local/bin"
    export PATH
    coveralls --gcov-options '\-lp'
fi
