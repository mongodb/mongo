# Usage: ./update.sh <double-conversion-src-directory>
#
# Copies the needed files from a directory containing the original
# double-conversion source that we need.

# This was last updated with git rev 04cae7a8d5ef3d62ceffb03cdc3d38f258457a52.

set -e

cp $1/LICENSE ./
cp $1/README ./

# Includes
cp $1/src/*.h ./

# Source
cp $1/src/*.cc ./

patch -p3 < add-mfbt-api-markers.patch
patch -p3 < use-StandardInteger.patch
patch -p3 < use-mozilla-assertions.patch
patch -p3 < use-static_assert.patch
patch -p3 < ToPrecision-exponential.patch
