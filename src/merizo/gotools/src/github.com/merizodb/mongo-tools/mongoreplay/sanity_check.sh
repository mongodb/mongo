#!/bin/bash

PORT=27017
STARTMERIZO=false
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PCAPFILE="$SCRIPT_DIR/merizoreplay_test.out"

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
		-m|--start-merizo)
			shift
			STARTMERIZO=true
			;;
		*)
			echo "Unknown arg: $1"
			exit 1
	esac
done

command -v merizoreplay >/dev/null
if [ $? != 0 ]; then
  echo "merizoreplay must be in PATH"
  exit 1
fi

set -e
set -o verbose

OUTFILE="$(echo $PCAPFILE | cut -f 1 -d '.').playback"
merizoreplay record -f $PCAPFILE -p $OUTFILE

if [ "$STARTMERIZO" = true ]; then
	rm -rf /data/merizoreplay/
	mkdir /data/merizoreplay/
	echo "starting MERIZOD"
	merizod --port=$PORT --dbpath=/data/merizoreplay &
	MERIZOPID=$!
fi

merizo --port=$PORT merizoplay_test --eval "db.setProfilingLevel(2);" 
merizo --port=$PORT merizoplay_test --eval "db.createCollection('sanity_check', {});" 

export MERIZOREPLAY_HOST="merizodb://localhost:$PORT"
merizoreplay play -p $OUTFILE
merizo --port=$PORT merizoplay_test --eval "var profile_results = db.system.profile.find({'ns':'merizoplay_test.sanity_check'});
assert.gt(profile_results.size(), 0);" 

merizo --port=$PORT merizoplay_test --eval "var query_results = db.sanity_check.find({'test_success':1});
assert.gt(query_results.size(), 0);" 

# test that files are correctly gziped ( TOOLS-1503 )
merizoreplay record -f $PCAPFILE -p ${OUTFILE} --gzip
gunzip -t ${OUTFILE}

echo "Success!"

if [ "$STARTMERIZO" = true ]; then
	kill $MERIZOPID
fi


