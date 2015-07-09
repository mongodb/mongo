#!/usr/bin/env bash

# Format in-tree documentation for posting to MDN.
# See js/src/doc/README.md for general usage information.
#
# Usage:
#
#   ./format.sh [--mdn] SOURCEDIR OUTPUTDIR
#
# Pages are tagged with the current Mercurial parent changeset ID.
#
# Normally, the generated HTML includes appropriate headers for in-place
# viewing, and the links are set up to allow the files to refer to each
# other where they're placed in OUTPUTDIR. However, if the --mdn flag is
# given, we omit non-body elements from the HTML (which seems to be what
# MDN prefers), and generate links to work with the URLs at which the pages
# will be published on MDN.

set -eu

progname=$(basename $0)
lib=$(cd $(dirname $0)/lib; pwd)

# If we're producing text meant to be reviewed locally, then ask Pandoc to
# produce standalone HTML files (which include text encoding metadata and
# other helpful things).
standalone_arg=--standalone

# If we're producing text for MDN, then generate links using the wiki URLs.
mdn_arg=

while true; do
    case "${1-}" in
        '--mdn')
            mdn_arg=--mdn
            standalone_arg=
            shift
            ;;
        *)
            break
            ;;
    esac
done

sourcedir=$1
outputdir=$2

config=$sourcedir/config.sh
if ! [ -f "$config" ]; then
    echo "SOURCEDIR doesn't seem to contain a 'config.sh' file: $sourcedir" >&2
    exit 1
fi

export JS_DOC_HG_IDENTIFY="$(hg identify | sed -e 's/ .*$//')"

# Compute the name of the source directory relative to the hg root, for the
# "this text computed from..." message.
hg_relative_sourcedir=$((cd $sourcedir; pwd) | sed -e "s|$(hg root)/||")

checked_pandoc=false

source $lib/dummy-config.sh

markdown() {
    INPUT_FILE=$1
    URL=$BASE_URL$2

    if ! $checked_pandoc; then
        if ! pandoc -v > /dev/null; then
            echo "$progname: This script uses the 'pandoc' formatter, but that doesn't seem" >&2
            echo "to be installed." >&2
            exit 1
        fi
        checked_pandoc=true
    fi

    local file=$sourcedir/$INPUT_FILE
    if ! [ -f "$file" ]; then
        echo "$progname: Can't find markdown file $file, mentioned by $config" >&2
        exit 1
    fi

    local output_file=$outputdir/${INPUT_FILE/md/html}

    mkdir -p $(dirname "$output_file")
    pandoc $standalone_arg                                              \
           -f markdown --smart -t html                                  \
           "$file"                                                      \
           <("$lib/make-bibliography.sh" $mdn_arg "$config" "$URL")     \
           -o "$output_file"

    "$lib/make-watermark.sh" "$output_file" "$hg_relative_sourcedir/$INPUT_FILE" >> "$output_file"
}

resource() {
    local label=$1 file=$2 url=$3
    ln -f $sourcedir/$file $outputdir/$file
}

source "$config"
