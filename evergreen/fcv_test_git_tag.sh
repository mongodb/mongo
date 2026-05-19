#!/bin/bash

# For FCV testing only.
# Output the version from .bazelrc.target_mongo_version before running tests.

set -o errexit
set -o verbose

cd src
echo "common --define=MONGO_VERSION=5.1.0-alpha" >.bazelrc.target_mongo_version
version=$(awk -F'MONGO_VERSION=' '/MONGO_VERSION=/ { split($2, version, /[[:space:]]/); print version[1]; exit }' .bazelrc.target_mongo_version)
if [[ -z "${version}" ]]; then
    echo "Unable to extract MONGO_VERSION from .bazelrc.target_mongo_version" >&2
    exit 1
fi
echo "r${version}"
