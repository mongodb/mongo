
#!/bin/bash
# This script downloads and imports opentelemetry-cpp.
#
# Turn on strict error checking, like perl use 'strict'
set -xeuo pipefail
IFS=$'\n\t'

NAME="opentelemetry-cpp"

VERSION="1.24"
BRANCH="mongo/v${VERSION}"

LIB_GIT_URL="https://github.com/mongodb-forks/opentelemetry-cpp.git"
LIB_GIT_DIR=$(mktemp -d /tmp/import-opentelemetry-cpp.XXXXXX)

trap "rm -rf $LIB_GIT_DIR" EXIT

LIBDIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME
DIST=${LIBDIR}/dist
git clone "$LIB_GIT_URL" $LIB_GIT_DIR
git -C $LIB_GIT_DIR checkout $BRANCH

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

## Remove all CMakelists.txt files
find $DIST/ -name "CMakeLists.txt" -delete

# Remove unneeded directories
pushd $DIST

# Remove unneeded log functionalities
rm exporters/otlp/include/opentelemetry/exporters/otlp/*log*.h
rm exporters/otlp/src/*log*.cc
rm -rf api/include/opentelemetry/logs
rm -rf sdk/include/opentelemetry/sdk/logs
rm -rf sdk/src/logs

# Unneeded exporters
rm -rf exporters/elasticsearch
rm -rf exporters/etw
rm -rf exporters/ostream
rm -rf exporters/zipkin

# Unneeded configuration files
rm -rf sdk/include/opentelemetry/sdk/configuration/
rm -rf sdk/src/configuration/

# Only keep the prometheus exporter utils
find exporters/prometheus/include/opentelemetry/exporters/prometheus/ -maxdepth 1 -type f \
    ! -name "exporter_utils.*" -delete
find exporters/prometheus/src/ -maxdepth 1 -type f ! -name "exporter_utils.*" -delete

# Test directories
rm -rf api/test
rm -rf exporters/otlp/test
rm -rf exporters/prometheus/test
rm -rf ext/test
rm -rf sdk/test
popd

cp -R ${DIST}/* ${LIBDIR}/
echo ${DIST}
rm -rf ${DIST}

PATCHES_DIR="${LIBDIR}/patches"
git apply "${PATCHES_DIR}/0001-compile-1.24.patch"
git apply "${PATCHES_DIR}/0002-SERVER-117299-prometheus-utils.patch"
git apply "${PATCHES_DIR}/0003-SERVER-131811-leak-statics.patch"
