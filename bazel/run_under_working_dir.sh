#!/bin/bash

set -e

RUNFILES_WORKING_DIRECTORY="$(pwd)"

if [ -z $BUILD_WORKING_DIRECTORY ]; then
    echo "ERROR: BUILD_WORKING_DIRECTORY was not set, was this run from bazel?"
    exit 1
fi

cd $BUILD_WORKING_DIRECTORY

${RUNFILES_WORKING_DIRECTORY}/${BINARY_PATH} "${@:1}"
