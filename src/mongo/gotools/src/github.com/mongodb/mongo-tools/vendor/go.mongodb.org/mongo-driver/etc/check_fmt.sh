#!/usr/bin/env bash

gofmt_out="$(gofmt -l -s "$@")"

if [[ $gofmt_out ]]; then
  echo "gofmt check failed for:";
  sed -e 's/^/ - /' <<< "$gofmt_out";
  exit 1;
fi
