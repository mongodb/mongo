#!/usr/bin/env bash

# Generate a Markdown bibliography file --- that is, definitions for
# reference-style link labels --- from a documentation directory's
# config.sh file.
#
# Usage:
#   make-bibliography.sh [--mdn] CONFIG CITING-URL
#
# where:
# - CONFIG is the name of the config.sh file to process; and
# - CITING-URL is the absolute URL at which the document using these labels
#   will appear. The links we output use URLs relative to CITING-URL to
#   refer to the URLs given in CONFIG.
#
# If given the --mdn flag, generate links that are correct for files
# as they appear in format.sh's OUTPUTDIR, not for publishing on MDN.

set -eu

mdn=false

while true; do
    case "${1-}" in
        '--mdn')
            mdn=true
            shift
            ;;
        *)
            break
            ;;
    esac
done

lib=$(dirname $0)
config=$1
citing=$2

source "$lib/common.sh"

source "$lib/dummy-config.sh"

label() {
    local label=$1; shift
    local fragment=
    case "$1" in
        '#'*)
            fragment=$1; shift
            ;;
    esac
    local title=${1:+ \"$1\"}

    citing_prefix=$(dirname "$citing")/
    if $mdn; then
        echo "[$label]: $(relative "$citing_prefix" "$BASE_URL$RELATIVE_URL")$fragment$title"
    else
        echo "[$label]: ${INPUT_FILE/md/html}$fragment$title"
    fi
}

absolute-label() {
    local label=$1
    local absolute_url=$2
    local title=$3

    echo "[$label]: $absolute_url${title:+ \"$title\"}"
}

resource() {
    local label=$1 file=$2 absolute_url=$3

    if [ -n "$label" ]; then
        if $mdn; then
            echo "[$label]: $absolute_url"
        else
            echo "[$label]: $file"
        fi
    fi
}

source "$config"
