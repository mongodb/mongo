#!/usr/bin/env bash

# Format js/src/doc documentation in SOURCEDIR, place formatted files in OUTPUTDIR,
# and post all changed pages to MDN using KEYID and SECRET to identify the poster.
# See js/src/doc/README.md for general usage information.
#
# Usage:
#
#   ./publish.sh SOURCEDIR OUTPUTDIR KEYID SECRET
#
# Pages are tagged with the current Mercurial parent changeset ID.

set -eu

progname=$(basename $0)
doc=$(cd $(dirname $0); pwd)
lib=$doc/lib

sourcedir=$1
outputdir=$2
keyid=$3
secret=$4

$doc/format.sh --mdn "$sourcedir" "$outputdir"

config=$sourcedir/config.sh

watermark=$lib/extract-watermark.sh

# Fetch a URL, with caching disabled.
fetch() {
    curl --silent -XGET "$1"                                    \
         -H"Cache-Control: no-cache, no-store, must-revalidate" \
         -H"Pragma: no-cache"                                   \
         -H"Expires: 0"
}

source $lib/dummy-config.sh

markdown() {
    INPUT_FILE=$1
    URL=$BASE_URL$2

    local formatted_file=$outputdir/${INPUT_FILE/md/html}

    # Extract the watermark from the formatted file.
    local local_watermark=$("$watermark" < "$formatted_file")

    # Get the existing page, and extract its watermark, if any.
    local public_watermark=$(fetch "$URL?raw" | "$watermark")

    if [ "$local_watermark" != "$public_watermark" ]; then
        echo "$progname: Updating: $URL" >&2
        local status
        status=$(curl --silent -X PUT -H"Content-Type: text/html" --upload-file "$formatted_file" -u "$keyid:$secret" "$URL")
        case "$status" in
            CREATED | RESET)
                ;;
            *)
                echo "$progname: Error posting $URL, from $config: $status" >&2
                exit 1
                ;;
        esac
    else
        echo "$progname: Unchanged: $URL" >&2
    fi
}

# MDN can't currently update attached resources. But we can verify that the current
# published versions match what we have.
resource() {
    local label=$1
    local file=$sourcedir/$2
    local url=$3

    if cmp "$file" <(fetch "$url") > /dev/null; then
        echo "$progname: Unchanged: $url" >&2
    else
        echo "$progname: Warning: resource out of date: $url" >&2
    fi
}

source $config
