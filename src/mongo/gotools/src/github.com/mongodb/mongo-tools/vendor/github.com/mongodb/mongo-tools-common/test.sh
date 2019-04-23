#!/bin/bash

tags=""
if [ ! -z "$1" ]
  then
  	tags="$@"
fi

# make sure we're in the directory where the script lives
SCRIPT_DIR="$(cd "$(dirname ${BASH_SOURCE[0]})" && pwd)"
cd $SCRIPT_DIR

export COVERAGE_ARGS=""

export TOOLS_TESTING_UNIT="true"
export TOOLS_TESTING_INTEGRATION="true"
./runTests.sh "$tags"
ec=$?
exit "$ec"
