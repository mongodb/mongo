#!/bin/bash
# This script downloads the mongodb/mongo-perf repository and generates mongoebench-compatible JSON
# config files equivalent to the JavaScript test cases.
#
# Turn on strict error checking, like perl use 'strict'.
set -xeuo pipefail
IFS=$'\n\t'

if [ "$#" -ne 0 ]; then
    echo "This script does not take any arguments"
    exit 1
fi

# We add the current working directory to the PATH because that is where the mongo shell may have
# been installed. The benchrun.py script is going to run the mongo shell binary that's on the PATH.
PATH=$PATH:$(pwd)

# The mongo shell processes spawned by the benchrun.py script will attempt to connect to a mongod,
# so we just error out if we find that one isn't already running.
mongo --eval 'db.adminCommand({ping: 1})' && rc=$? || rc=$?
if [ "$rc" -ne 0 ]; then
    echo "This script requires a mongod to be running on port 27017"
    exit 2
fi

NAME=mongo-perf
SRC_ROOT=$(mktemp -d /tmp/$NAME.XXXXXX)
trap "rm -rf $SRC_ROOT" EXIT
DEST_DIR=$(git rev-parse --show-toplevel)/benchrun_embedded/testcases

git clone --branch=master https://github.com/mongodb/mongo-perf.git $SRC_ROOT

pushd $SRC_ROOT

# We pin to a particular commit of the mongodb/mongo-perf repository to make it clear what version
# of the JavaScript test cases we are running.
git checkout 824f1672b4e684ea4fdb4399e14374c5969467f9

# We use Python to get the number of CPUs in a platform-agnostic way.
NUM_CPUS=$(python -c 'import multiprocessing; print(multiprocessing.cpu_count())')

# Generating the JSON config files sequentially takes ~6 minutes due to how certain JavaScript test
# cases build up very large documents only to eventually realize upon trying to serialize them out
# to a file that they are too big. We use `xargs -P` to speed up generating the JSON config files.
#
# We don't generate JSON config files for tests that are tagged with "capped" or "where" because
# they aren't supported by embedded.
find testcases -type f -print0 | xargs -0 -I% -n1 -P$NUM_CPUS  \
    python2 benchrun.py --testfiles %                          \
                        --threads 1                            \
                        --excludeFilter capped                 \
                        --excludeFilter where                  \
                        --generateMongoeBenchConfigFiles mongoebench/

popd

test -d $DEST_DIR && rm -r $DEST_DIR
mkdir -p $(dirname $DEST_DIR)

mv $SRC_ROOT/mongoebench $DEST_DIR
