#!/bin/sh
# list_pkgs <directory>
directory="$1"
if [ -z "$directory" ]; then
    directory="."
fi

if [ "Windows_NT" = "$OS" ]; then
    find "$directory" -iname "*_test.go" | perl -ple 's{(.*)/[^/]+}{$1}' | sort -u
else
    go list -tags cse -test -f '{{.ForTest}}' $directory/... | sed -e "s/^go.mongodb.org\/mongo-driver/./"
fi
