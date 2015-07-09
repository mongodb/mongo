#!/usr/bin/env bash

# Generate an HTML watermark for the given file. Claim the text was
# generated from SOURCE.
#
# Usage:
#
#   make-watermark.sh FILE SOURCE

set -eu

# Include the watermark as visible text in the page, as suggested in bug 990662.

echo '<h4>Source Metadata</h4>'
echo '<dl>'
echo '    <dt>Generated from file:<dt>'
echo "    <dd>$2</dd>"
echo '    <dt>Watermark:</dt>'
echo "    <dd id='watermark'>sha256:$(cat "$1" | shasum -a 256 | sed 's/ .*$//')</dd>"

# If we have Mercurial changeset ID, include it.
if [ "${JS_DOC_HG_IDENTIFY:+set}" = set ]; then
    # If the changeset ID has a '+' on the end (indicating local
    # modifications), omit that from the link.
    cat <<EOF
    <dt>Changeset:</dt>
    <dd><a href="https://hg.mozilla.org/mozilla-central/rev/${JS_DOC_HG_IDENTIFY%+}">${JS_DOC_HG_IDENTIFY}</a></dd>
EOF
fi

echo '</dl>'
