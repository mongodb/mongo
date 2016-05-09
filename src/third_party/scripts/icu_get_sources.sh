#!/bin/bash

# This script fetches sources for ICU and builds custom ICU data files (one big-endian data file
# and one-little endian data file).  Both the sources and the data files are trimmed down for size.
#
# This script can be run from anywhere within the root of the source repository.  This script
# expects the ICU third-party directory (src/third_party/icu4c-xx.yy/) to exist and contain a
# newline-separated language file in source/mongo_sources/languages.txt.  This language file must
# list each locale for which collation data should be packaged as part of the generated custom data
# file.
#
# This script returns a zero exit code on success.

set -euo pipefail
IFS=$'\n\t'

if [ "$#" -ne 0 ]; then
    echo "$0: too many arguments" >&2
    exit 1
fi

KERNEL="$(uname)"
if [ "$KERNEL" != Linux ]; then
    echo "$0: kernel '$KERNEL' not supported" >&2
    exit 1
fi

NAME=icu4c
MAJOR_VERSION=57
MINOR_VERSION=1
VERSION="${MAJOR_VERSION}.${MINOR_VERSION}"

TARBALL="${NAME}-${MAJOR_VERSION}_${MINOR_VERSION}-src.tgz"
TARBALL_DOWNLOAD_URL="http://download.icu-project.org/files/${NAME}/${VERSION}/${TARBALL}"

ICU_THIRD_PARTY_DIR="$(git rev-parse --show-toplevel)/src/third_party/${NAME}-${VERSION}"
MONGO_SOURCES_DIR="${ICU_THIRD_PARTY_DIR}/source/mongo_sources"
LANGUAGE_FILE_IN="${MONGO_SOURCES_DIR}/languages.txt"
ICU_DATA_FILE_LITTLE_ENDIAN_OUT="${MONGO_SOURCES_DIR}/icudt${MAJOR_VERSION}l.dat"
ICU_DATA_FILE_BIG_ENDIAN_OUT="${MONGO_SOURCES_DIR}/icudt${MAJOR_VERSION}b.dat"

#
# Set up temp directory.
#

TEMP_DIR="$(mktemp -d /tmp/icu.XXXXXX)"
trap "rm -rf $TEMP_DIR" EXIT

TARBALL_DIR="${TEMP_DIR}/tarball"
INSTALL_DIR="${TEMP_DIR}/install"
DATA_DIR="${TEMP_DIR}/data"
mkdir "$TARBALL_DIR" "$INSTALL_DIR" "$DATA_DIR"

#
# Download and extract tarball into temp directory.
#

cd "$TEMP_DIR"
wget "$TARBALL_DOWNLOAD_URL"
tar --strip-components=1 -C "$TARBALL_DIR" -zxf "$TARBALL"

#
# Build and install ICU in temp directory, in order to use data packaging tools.
#

cd "${TARBALL_DIR}/source"
./runConfigureICU "$KERNEL" --prefix="${TEMP_DIR}/install"
make -j
make install

#
# Generate trimmed-down list of data to include in custom data files.
#

ORIGINAL_DATA_FILE="${TARBALL_DIR}/source/data/in/icudt${MAJOR_VERSION}l.dat"
ORIGINAL_DATA_LIST="${DATA_DIR}/icudt${MAJOR_VERSION}l.lst.orig"
NEW_DATA_LIST="${DATA_DIR}/icudt${MAJOR_VERSION}l.lst"

LD_LIBRARY_PATH= eval $("${INSTALL_DIR}/bin/icu-config" --invoke=icupkg) -l "$ORIGINAL_DATA_FILE" \
    > "$ORIGINAL_DATA_LIST"

DESIRED_DATA_DIRECTORIES="coll"
BASE_FILES="root.res
ucadata.icu"
for DESIRED_DATA_DIRECTORY in $DESIRED_DATA_DIRECTORIES; do
    for BASE_FILE in $BASE_FILES; do
        # Using grep to sanity-check that the file indeed appears in the original data list.
        grep -E "^${DESIRED_DATA_DIRECTORY}/${BASE_FILE}$" "$ORIGINAL_DATA_LIST" >> "$NEW_DATA_LIST"
    done
    for LANGUAGE in $(grep -Ev "^#" "$LANGUAGE_FILE_IN"); do
        # Ditto above.
        grep -E "^${DESIRED_DATA_DIRECTORY}/${LANGUAGE}.res$" "$ORIGINAL_DATA_LIST" \
            >> "$NEW_DATA_LIST"
    done
done

#
# Extract desired data, and use it to build custom data files.
#

LD_LIBRARY_PATH= eval $("${INSTALL_DIR}/bin/icu-config" --invoke=icupkg) -d "$DATA_DIR" \
    -x "$NEW_DATA_LIST" "$ORIGINAL_DATA_FILE"
LD_LIBRARY_PATH= eval $("${INSTALL_DIR}/bin/icu-config" --invoke=icupkg) -s "$DATA_DIR" \
    -a "$NEW_DATA_LIST" -tl new "$ICU_DATA_FILE_LITTLE_ENDIAN_OUT"
LD_LIBRARY_PATH= eval $("${INSTALL_DIR}/bin/icu-config" --invoke=icupkg) -s "$DATA_DIR" \
    -a "$NEW_DATA_LIST" -tb new "$ICU_DATA_FILE_BIG_ENDIAN_OUT"

#
# Re-extract pristine sources into final destination, prune unneeded sources.
#

tar --strip-components=1 -C "$ICU_THIRD_PARTY_DIR" -zxf "${TEMP_DIR}/${TARBALL}"
rm -f ${ICU_THIRD_PARTY_DIR}/source/*.in             # Build system.
rm -f ${ICU_THIRD_PARTY_DIR}/source/*.m4             # Build system.
rm -f ${ICU_THIRD_PARTY_DIR}/source/install-sh       # Build system.
rm -f ${ICU_THIRD_PARTY_DIR}/source/mkinstalldirs    # Build system.
rm -f ${ICU_THIRD_PARTY_DIR}/source/runConfigureICU  # Build system.
rm -rf ${ICU_THIRD_PARTY_DIR}/as_is/                 # Scripts.
rm -rf ${ICU_THIRD_PARTY_DIR}/source/allinone/       # Workspace and project files.
rm -rf ${ICU_THIRD_PARTY_DIR}/source/config*         # Build system.
rm -rf ${ICU_THIRD_PARTY_DIR}/source/data/           # Source data.
rm -rf ${ICU_THIRD_PARTY_DIR}/source/extra/          # Non-supported API additions.
rm -rf ${ICU_THIRD_PARTY_DIR}/source/io/             # ICU I/O library.
rm -rf ${ICU_THIRD_PARTY_DIR}/source/layout/         # ICU complex text layout engine.
rm -rf ${ICU_THIRD_PARTY_DIR}/source/layoutex/       # ICU paragraph layout engine.
rm -rf ${ICU_THIRD_PARTY_DIR}/source/samples/        # Sample programs.
rm -rf ${ICU_THIRD_PARTY_DIR}/source/test/           # Test suites.
rm -rf ${ICU_THIRD_PARTY_DIR}/source/tools/          # Tools for generating the data files.
