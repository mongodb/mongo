#!/bin/bash

# For FCV testing only.
# Output the version from .bazelrc.target_mongo_version before running tests.

set -o errexit
set -o verbose

cd src
echo "common --define=MONGO_VERSION=5.1.0-alpha" >.bazelrc.target_mongo_version
echo "r$(grep -oP '(?<=MONGO_VERSION=)[^\s]+' .bazelrc.target_mongo_version)"
