#!/bin/bash
# This script downloads and imports opentelemetry-proto.
#
# Turn on strict error checking, like perl use 'strict'
set -xeuo pipefail
IFS=$'\n\t'

NAME="opentelemetry-proto"

VERSION="1.3.2"
BRANCH="mongo/v${VERSION}"

LIB_GIT_URL="https://github.com/mongodb-forks/opentelemetry-proto.git"
LIB_GIT_DIR=$(mktemp -d /tmp/import-opentelemetry-proto.XXXXXX)

trap "rm -rf $LIB_GIT_DIR" EXIT

LIBDIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME
DIST=${LIBDIR}/dist
git clone "$LIB_GIT_URL" $LIB_GIT_DIR
git -C $LIB_GIT_DIR checkout ${BRANCH}

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME

SUBDIR_WHITELIST=(
    opentelemetry
    LICENSE
    README.md
)

for subdir in ${SUBDIR_WHITELIST[@]}
do
    [[ -d $LIB_GIT_DIR/$subdir ]] && mkdir -p $DIST/$subdir
    cp -Trp $LIB_GIT_DIR/$subdir $DIST/$subdir
done

PATCHES_DIR="${LIBDIR}/patches"
git apply "${PATCHES_DIR}/0001-Add-build-system.patch"
git apply "${PATCHES_DIR}/0001-SERVER-105009-add-metrics-to-opentelemetry-proto-BUI.patch"
git apply "${PATCHES_DIR}/0001-SERVER-100604-update-otel_rules.bzl-with-intermediat.patch"
git apply "${PATCHES_DIR}/0001-SERVER-106258-use-strip_import_prefix-on-OTel-proto-.patch"
git apply "${PATCHES_DIR}/0001-SERVER-106258-vendor-OpenTelemetry-gRPC-exporter.patch"

## 2. Move code from src/third_party/opentelemetry-proto/dist/ to src/third_party/opentelemetry-proto/
cp -R ${DIST}/* ${LIBDIR}/
echo ${DIST}
rm -rf ${DIST}
