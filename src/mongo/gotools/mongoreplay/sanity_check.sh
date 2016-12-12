#!/bin/bash

PORT=27017
STARTMONGO=false
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PCAPFILE="$SCRIPT_DIR/mongoreplay_test.out"

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
		-m|--start-mongo)
			shift
			STARTMONGO=true
			;;
		*)
			echo "Unknown arg: $1"
			exit 1
	esac
done

command -v mongoreplay >/dev/null
if [ $? != 0 ]; then
  echo "mongoreplay must be in PATH"
  exit 1
fi

set -e
set -o verbose

OUTFILE="$(echo $PCAPFILE | cut -f 1 -d '.').playback"
mongoreplay record -f $PCAPFILE -p $OUTFILE

if [ "$STARTMONGO" = true ]; then
	rm -rf /data/mongoreplay/
	mkdir /data/mongoreplay/
	echo "starting MONGOD"
	mongod --port=$PORT --dbpath=/data/mongoreplay &
	MONGOPID=$!
fi

mongo --port=$PORT mongoplay_test --eval "db.setProfilingLevel(2);" 
mongo --port=$PORT mongoplay_test --eval "db.createCollection('sanity_check', {});" 

mongoreplay play --host mongodb://localhost:$PORT -p $OUTFILE
mongo --port=$PORT mongoplay_test --eval "var profile_results = db.system.profile.find({'ns':'mongoplay_test.sanity_check'});
assert.gt(profile_results.size(), 0);" 

mongo --port=$PORT mongoplay_test --eval "var query_results = db.sanity_check.find({'test_success':1});
assert.gt(query_results.size(), 0);" 

# test that files are correctly gziped ( TOOLS-1503 )
mongoreplay record -f $PCAPFILE -p ${OUTFILE} --gzip
gunzip -t ${OUTFILE}

echo "Success!"

if [ "$STARTMONGO" = true ]; then
	kill $MONGOPID
fi


