#!/bin/bash

# Usage: ./update.sh [<git-rev-to-use>]
#
# Copies the needed files from a directory containing the original
# double-conversion source that we need.  If no revision is specified, the tip
# revision is used.  See GIT-INFO for the last revision used.

set -e

LOCAL_PATCHES=""

LOCAL_PATCHES="$LOCAL_PATCHES add-mfbt-api-markers.patch"
LOCAL_PATCHES="$LOCAL_PATCHES use-StandardInteger.patch"
LOCAL_PATCHES="$LOCAL_PATCHES use-mozilla-assertions.patch"
LOCAL_PATCHES="$LOCAL_PATCHES ToPrecision-exponential.patch"

TMPDIR=`mktemp --directory`
LOCAL_CLONE="$TMPDIR/new-double-conversion"

git clone https://github.com/google/double-conversion.git "$LOCAL_CLONE"

# If a particular revision was requested, check it out.
if [ "$1" !=  "" ]; then
  git -C "$LOCAL_CLONE" checkout "$1"
fi

# First clear out everything already present.
DEST=./double-conversion
mv "$DEST" "$TMPDIR"/old-double-conversion
mkdir "$DEST"

# Copy over critical files.
cp "$LOCAL_CLONE/LICENSE" "$DEST/"
cp "$LOCAL_CLONE/README.md" "$DEST/"

# Includes
for header in "$LOCAL_CLONE/double-conversion/"*.h; do
  cp "$header" "$DEST/"
done

# Source
for ccfile in "$LOCAL_CLONE/double-conversion/"*.cc; do
  cp "$ccfile" "$DEST/"
done

# Now apply our local patches.
for patch in $LOCAL_PATCHES; do
  patch --directory "$DEST" --strip 4 < "$patch"

  # Out-of-date patches may spew *.{orig,rej} when applied.  Report an error if
  # any such file is found, and roll the source directory back to its previous
  # state in such case.
  detritus_files=`find "$DEST" -name '*.orig' -o -name '*.rej'`
  if [ "$detritus_files" != "" ]; then
    echo "ERROR: Local patch $patch created these detritus files when applied:"
    echo ""
    echo "  $detritus_files"
    echo ""
    echo "Please fix $patch before running $0."

    rm -rf "$DEST"
    mv "$TMPDIR"/source "$DEST"

    exit 1
  fi
done

# Update Mercurial file status.
hg addremove "$DEST"

# Note the revision used in this update.
git -C "$LOCAL_CLONE" show > ./GIT-INFO

# Delete the tmpdir.
rm -rf "$TMPDIR"
