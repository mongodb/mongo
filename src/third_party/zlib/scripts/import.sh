#!/bin/bash
# Download and import zlib.
# License: Zlib
# Upstream: https://zlib.net/
#
# Copies only the library sources and top-level docs into this package.
# BUILD.bazel, MODULE.bazel, and scripts/ are MongoDB-owned and are left in
# place. Upstream ships its own BUILD.bazel and MODULE.bazel; those are not
# imported because the vendored BUILD.bazel loads cc_library from a different
# rules_cc path.

VERSION=1.4
NAME=zlib

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

TARBALL="${NAME}-${VERSION}.tar.gz"
URL="https://zlib.net/${TARBALL}"
# SHA-256 of this exact tarball. Replace it when VERSION changes.
SHA256=02dd159783677a749c04adbdd02b55b8a2bcc7e054f51b76b549199a916ad584

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

wget -O "${TMP_DIR}/${TARBALL}" "${URL}"
actual_sha256="$(sha256sum "${TMP_DIR}/${TARBALL}" | awk '{ print $1 }')"
if [[ "${actual_sha256}" != "${SHA256}" ]]; then
    echo "SHA-256 of ${TARBALL} is ${actual_sha256}, expected ${SHA256}" >&2
    exit 1
fi

tar -zxf "${TMP_DIR}/${TARBALL}" -C "${TMP_DIR}"

SRC_DIR="${TMP_DIR}/${NAME}-${VERSION}"
if [[ ! -d "${SRC_DIR}" ]]; then
    echo "Expected ${TARBALL} to extract to ${NAME}-${VERSION}/, but that directory is missing" >&2
    exit 1
fi

shopt -s nullglob
sources=("${SRC_DIR}"/*.{c,h})
docs=(README FAQ INDEX LICENSE)
for doc in "${docs[@]}"; do
    if [[ ! -f "${SRC_DIR}/${doc}" ]]; then
        echo "Expected ${NAME}-${VERSION}/${doc} in ${TARBALL}, but it is missing" >&2
        exit 1
    fi
done
if [[ ${#sources[@]} -eq 0 ]]; then
    echo "Expected C sources in ${NAME}-${VERSION}/, but ${TARBALL} has none" >&2
    exit 1
fi

# Replace previously imported sources only after the archive layout checks out.
# BUILD.bazel, MODULE.bazel, and scripts/ are MongoDB-owned and stay in place.
rm -f "${DEST_DIR}"/*.{c,h}
rm -f "${DEST_DIR}/README" "${DEST_DIR}/FAQ" "${DEST_DIR}/INDEX" "${DEST_DIR}/LICENSE"

cp "${sources[@]}" "${DEST_DIR}/"
cp "${docs[@]/#/${SRC_DIR}/}" "${DEST_DIR}/"

echo "zlib ${VERSION} imported into ${DEST_DIR}"
