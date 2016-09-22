#!/bin/bash

PORT=27017
PCAPFILE="mongoreplay_test.out"
STARTMONGO=false

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

OUTFILE="$(echo $PCAPFILE | cut -f 1 -d '.').playback"
mongoreplay record --silent -f $PCAPFILE -p $OUTFILE

if [ "$STARTMONGO" = true ]; then
	rm -rf /data/mongoreplay/
	mkdir /data/mongoreplay/
	echo "starting MONGOD"
	mongod --port=$PORT --dbpath=/data/mongoreplay > /dev/null 2>&1 &
	MONGOPID=$!
fi

mongo --port=$PORT mongoplay_test --eval "db.setProfilingLevel(2);" >/dev/null
mongo --port=$PORT mongoplay_test --eval "db.createCollection('sanity_check', {});" >/dev/null

mongoreplay play --silent --collect=none -p $OUTFILE
mongo --port=$PORT mongoplay_test --eval "var profile_results = db.system.profile.find({'ns':'mongoplay_test.sanity_check'});
assert.gt(profile_results.size(), 0);" >/dev/null

mongo --port=$PORT mongoplay_test --eval "var query_results = db.sanity_check.find({'test_success':1});
assert.gt(query_results.size(), 0);" >/dev/null
echo "Success!"

if [ "$STARTMONGO" = true ]; then
	kill $MONGOPID
fi


