#!/bin/bash

PORT=27017
STARTBONGO=false
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PCAPFILE="$SCRIPT_DIR/bongoreplay_test.out"

while test $# -gt 0; do
	case "$1" in
		-f|--file)
			shift
			PCAPFILE="$1"
			shift
			;;
		-p|--port)
			shift
			PORT="$1"
			shift
			;;
		-m|--start-bongo)
			shift
			STARTBONGO=true
			;;
		*)
			echo "Unknown arg: $1"
			exit 1
	esac
done

command -v bongoreplay >/dev/null
if [ $? != 0 ]; then
  echo "bongoreplay must be in PATH"
  exit 1
fi

set -e
set -o verbose

OUTFILE="$(echo $PCAPFILE | cut -f 1 -d '.').playback"
bongoreplay record -f $PCAPFILE -p $OUTFILE

if [ "$STARTBONGO" = true ]; then
	rm -rf /data/bongoreplay/
	mkdir /data/bongoreplay/
	echo "starting BONGOD"
	bongod --port=$PORT --dbpath=/data/bongoreplay &
	BONGOPID=$!
fi

bongo --port=$PORT bongoplay_test --eval "db.setProfilingLevel(2);"
bongo --port=$PORT bongoplay_test --eval "db.createCollection('sanity_check', {});"

bongoreplay play --host bongodb://localhost:$PORT -p $OUTFILE
bongo --port=$PORT bongoplay_test --eval "var profile_results = db.system.profile.find({'ns':'bongoplay_test.sanity_check'});
assert.gt(profile_results.size(), 0);" 

bongo --port=$PORT bongoplay_test --eval "var query_results = db.sanity_check.find({'test_success':1});
assert.gt(query_results.size(), 0);" 

# test that files are correctly gziped ( TOOLS-1503 )
bongoreplay record -f $PCAPFILE -p ${OUTFILE} --gzip
gunzip -t ${OUTFILE}

echo "Success!"

if [ "$STARTBONGO" = true ]; then
	kill $BONGOPID
fi


