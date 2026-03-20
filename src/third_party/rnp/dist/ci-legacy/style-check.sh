#!/bin/bash
set -eu

echo Evaluating changes:
git diff -U0 "$TRAVIS_COMMIT_RANGE"
echo

diffs=$(git diff -U0 --no-color "$TRAVIS_COMMIT_RANGE" | "$CLANG_FORMAT_DIFF" -p1 -iregex '.*\.[ch]$')
if [ "$diffs" != "" ]; then
  echo ==== Style changes required ====
  echo "$diffs"
  exit 1
fi

