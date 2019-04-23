#!/bin/bash
set -o errexit

golint $(go list ./... | sed -e 's!github.com/mongodb/mongo-tools-common!.!') \
  | grep -v 'should have comment' \
  | grep -v 'comment on exported' \
  | grep -v 'Id.*should be.*ID'  \
  | perl -nle '$err = 1, print $_ if $_ } END { exit 1 if $err'

GOLINT_RC="${PIPESTATUS[0]}"
if [ $GOLINT_RC -ne 0 ]; then
  exit 1
fi
