
#!/bin/bash
# This script downloads and imports opentelemetry-cpp.
#
# Turn on strict error checking, like perl use 'strict'
set -xeuo pipefail
IFS=$'\n\t'

NAME="opentelemetry-cpp"

VERSION="mongo/v1.17"

LIB_GIT_URL="https://github.com/mongodb-forks/opentelemetry-cpp.git"
LIB_GIT_DIR=$(mktemp -d /tmp/import-opentelemetry-cpp.XXXXXX)

trap "rm -rf $LIB_GIT_DIR" EXIT

LIBDIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME
DIST=${LIBDIR}/dist
git clone "$LIB_GIT_URL" $LIB_GIT_DIR
git -C $LIB_GIT_DIR checkout $VERSION

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME

SUBDIR_WHITELIST=(
    api
    exporters
    ext
    sdk
    LICENSE
    README.md
)

for subdir in ${SUBDIR_WHITELIST[@]}
do
    [[ -d $LIB_GIT_DIR/$subdir ]] && mkdir -p $DIST/$subdir
    cp -Trp $LIB_GIT_DIR/$subdir $DIST/$subdir
done

## Remote all CMakelists.txt files
find $DIST/ -name "CMakeLists.txt" -delete

## Fix includes in headers and cpp files.
update_includes() {
    sed -i 's@#include "opentelemetry/proto@#include "src/third_party/opentelemetry-proto/opentelemetry/proto@' $file
    sed -i 's@#include "opentelemetry/sdk@#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk@' $file
    #sed -i 's@#include "opentelemetry/version.h"@#include "third_party/opentelemetry-cpp/api/include/opentelemetry/version.h"@' $file
}

for file in $(find $DIST/ -name "*.h" ); do
    update_includes "${file}"
done

for file in $(find $DIST/ -name "*.cc" ); do
    update_includes "${file}"
done

# Update  path to version.h
update_version() {
    sed -i 's@#include "opentelemetry/version.h"@#include "third_party/opentelemetry-cpp/api/include/opentelemetry/version.h"@' $file
}

for file in $(find $DIST/sdk -name "*.cc" ); do
    update_version "${file}"
done

for file in $(find $DIST/exporters -name "*.cc" ); do
    update_version "${file}"
done

# Remove uneeded directories
pushd $DIST

# Remove file uneeded functionalities (logs, metrics, grpc)
rm exporters/otlp/include/opentelemetry/exporters/otlp/*grpc*.h
rm exporters/otlp/include/opentelemetry/exporters/otlp/*log*.h
rm exporters/otlp/include/opentelemetry/exporters/otlp/*metric*.h
rm exporters/otlp/src/*grpc*.cc
rm exporters/otlp/src/*log*.cc
rm exporters/otlp/src/*metric*.cc
rm -rf api/include/opentelemetry/logs
rm -rf api/include/opentelemetry/metrics
rm -rf sdk/include/opentelemetry/sdk/logs
rm -rf sdk/include/opentelemetry/sdk/metrics
rm -rf sdk/src/logs
rm -rf sdk/src/metrics

# Uneeded  exporters
rm -rf exporters/elasticsearch
rm -rf exporters/etw
rm -rf exporters/memory
rm -rf exporters/ostream
rm -rf exporters/prometheus
rm -rf exporters/zipkin

# Test directories
rm -rf api/test
rm -rf exporters/otlp/test
rm -rf ext/test
rm -rf sdk/test
popd

## Manual steps to complete
## 1. Apply patch 0001-Build-system-changes-for-opentelemetry-cpp.patch
## 2. Move code from src/third_party/opentelemetry-cpp/dist/ to src/third_party/opentelemetry-cpp
## 3. Remove empty folder src/third_party/opentelemetry-cpp/dist/
