#!/bin/bash
# Publish missing manifest indices only after a complete, time-bounded scan.
probe=$1
budget=$2
count=$3
/usr/bin/timeout --kill-after=1s "${budget}s" /bin/bash -c '
    while [ ! -e "$1/ready" ]; do /bin/sleep 0.05; done
    exec >"$1/result.tmp" || exit 1
    index=0
    while IFS= read -r -d "" path; do
        [ -e "$path" ] || printf "%s\n" "$index"
        index=$((index + 1))
    done <"$1/manifest"
    [ "$index" -eq "$2" ] || exit 1
    printf "done %s\n" "$index"
' _ "$probe" "$count" &&
    /bin/mv "$probe/result.tmp" "$probe/result"
