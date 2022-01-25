#!/bin/bash

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

if [ "$(uname)" != "Linux" ]; then
  echo "Skipping test, this is for linux only"
  exit 0
fi

SOPATH="build/csfle-lib-${version}/lib/mongo_csfle_v1.so"
if [ ! -e "$SOPATH" ]; then
  echo "Error: can not find library at: $SOPATH"
  exit 1
fi

expect='A MONGO_CSFLE_1.0
T mongo_csfle_v1_analyze_query
T mongo_csfle_v1_bson_free
T mongo_csfle_v1_create
T mongo_csfle_v1_destroy
T mongo_csfle_v1_query_analyzer_create
T mongo_csfle_v1_query_analyzer_destroy
T mongo_csfle_v1_status_create
T mongo_csfle_v1_status_destroy
T mongo_csfle_v1_status_get_code
T mongo_csfle_v1_status_get_error
T mongo_csfle_v1_status_get_explanation'

actual="$(nm --extern-only --defined-only "$SOPATH" | awk '{ print $2, $3 }' | sort)"

if [ "$actual" != "$expect" ]; then
  echo "Error: symbols are not as expected in: $SOPATH"
  echo "Diff:"
  diff <(echo "$actual") <(echo "$expect")
  exit 1
fi
