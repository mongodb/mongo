#!/usr/bin/bash

SCRIPT_DIR=$(dirname $0)
SOURCE_ROOT="$SCRIPT_DIR/.."

function merged_decls_headers
{
    jq -r 'map(select(.mod as $mod | any(.used_from[]; .mod != $mod)) | .loc | split(":") | .[0]) | unique[]' $SOURCE_ROOT/merged_decls.json
}

function all_headers
{
    cd $SOURCE_ROOT && find src/mongo -name '*.h' | sort
}

# process substitution to avoid temporary files
diff -u <(merged_decls_headers) <(all_headers) | grep ^+ | cut -b 2-
