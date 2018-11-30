#!/bin/bash

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PORT=28080
INTERFACE=lo
KEEP=false
DBPATH=/data/mongoreplay
SILENT="--silent"
EXPLICIT=
VERBOSE=
DEBUG=
WORKLOADS=()
ASSERTIONS=()

log() {
  >&2 echo $@
}

while test $# -gt 0; do
  case "$1" in
    -h|--help)
      >&2 cat <<< "Usage: `basename $0` [OPTIONS]

-a, --assert JS-BOOL     condition for assertion after workload (used with -w);
                         can be specified in multiplicity
    --dbpath             path for mongod, can be cleared by program
                         (defaults to $DBPATH)
-e, --explicit           show comparison scores for all individual metrics
-i, --interface NI       network interface (defaults to $INTERFACE)
-k, --keep               keep temp files
-p, --port PORT          use port PORT (defaults to $PORT)
-v, --verbose            unsilence mongoreplay and make it slightly loud
-w, --workload JS-FILE   mongo shell workload script (used with -a);
                         runs concurrent workloads when specified in
                         multiplicity
"
      exit 1
      ;;
    -a|--assert)
      shift
      ASSERTIONS+=("$1")
      shift
      ;;
    -e|--explicit)
      shift
      EXPLICIT="--explicit"
      ;;
    -i|--interface)
      shift
      INTERFACE="$1"
      shift
      ;;
    -k|--keep)
      shift
      KEEP=true
      ;;
    -p|--port)
      shift
      PORT="$1"
      shift
      ;;
    -v|--verbose)
      shift
      SILENT=
      VERBOSE="-vv"
      DEBUG="-dd"
      ;;
    -w|--workload)
      shift
      WORKLOADS+=("$1")
      shift
      ;;
    --dbpath)
      shift
      DBPATH="$1"
      shift
      ;;
    *)
      log "Unknown arg: $1"
      exit 1
  esac
done


if [ ${#WORKLOADS[@]} -eq 0 ]; then
  # default workload/assert
  WORKLOADS=( "$SCRIPT_DIR/testWorkloads/crud.js" )
  MONGOASSERT=( 'db.bench.count() === 15000' )
elif [ ${#ASSERTIONS[@]} -eq 0 ]; then
  log "must specify BOTH -a/--assert AND -w/--workload"
  exit 1
fi

command -v mongoreplay >/dev/null
if [ $? != 0 ]; then
  log "mongoreplay must be in PATH"
  exit 1
fi
command -v ftdc >/dev/null
if [ $? != 0 ]; then
  log "ftdc command (github.com/10gen/ftdc-utils) must be in PATH"
  exit 1
fi

set -e

rm -rf $DBPATH
mkdir $DBPATH
log "starting MONGOD"
mongod --port=$PORT --dbpath=$DBPATH >/dev/null &
MONGOPID=$!

check_integrity() {
  set +e
  for ((i = 0; i < ${#ASSERTIONS[@]}; i++))
  do
    assertion="${ASSERTIONS[$i]}"
    mongo --port=$PORT --quiet mongoreplay_test --eval "assert($assertion)" >/dev/null
    STATUS=$?
    if [ $STATUS != 0 ]; then
      log "integrity check FAILED: $assertion"
      log "for further analysis, check db at localhost:$PORT, pid=$MONGOPID"
      exit 1
    fi
  done
  set -e
}

sleep 1
mongo --port=$PORT mongoreplay_test --eval "db.dropDatabase()" >/dev/null 2>&1

log "starting TCPDUMP"
sudo tcpdump -i "$INTERFACE" port "$PORT" -w tmp.pcap &
TCPDUMPPID=$!
sleep 1 # make sure it actually starts capturing

log "starting WORKLOAD"
START=`date`
sleep 1
# fork
WorkerPIDs=()
for ((i = 0; i < ${#WORKLOADS[@]}; i++))
do
  script="${WORKLOADS[$i]}"
  mongo --port=$PORT --quiet mongoreplay_test "$script" >/dev/null &
  WorkerPIDs+=("$!")
done
# join
for pid in "${WorkerPIDs[@]}"
do wait $pid
done
sleep 1
END=`date`
log "finished WORKLOAD"

log "stopping TCPDUMP"
( sleep 1 ; sudo kill $TCPDUMPPID) &
wait $TCPDUMPPID

check_integrity

# clean up database
mongo --port=$PORT mongoreplay_test --eval "db.dropDatabase()" >/dev/null 2>&1
sleep 1 # mongoreplay play should certainly happen after the drop

log "running mongoreplay RECORD"
mongoreplay record $SILENT $VERBOSE $DEBUG -f=tmp.pcap -p=tmp.playback >/dev/null
TAPECODE=$?
if [ "$TAPECODE" != 0 ]; then
  log "mongoreplay failed with code $TAPECODE"
  exit 1
fi

log # newline to separate replay

log "starting mongoreplay PLAY"
REPLAY_START=`date`
sleep 1
mongoreplay play $SILENT $VERBOSE $DEBUG --host "localhost:$PORT" -p=tmp.playback --report tmp.report >/dev/null
sleep 1
REPLAY_END=`date`
log "finished mongoreplay PLAY"

check_integrity

log "flushing FTDC diagnostic files (15 sec)"
sleep 15

log "killing MONGOD"
mongo --port=$PORT mongoreplay_test --eval "db.dropDatabase()" >/dev/null 2>&1
sleep 1
kill $MONGOPID
sleep 2 # give it a chance to dump FTDC

log "gathering FTDC statistics"
if $KEEP; then
  # dump raw ftdc
  ftdc decode -ms $DBPATH/diagnostic.data/* --start="$START" --end="$END" --out="tmp.base.json"
  ftdc decode -ms $DBPATH/diagnostic.data/* --start="$REPLAY_START" --end="$REPLAY_END" --out="tmp.play.json"
fi

log "base time START: $START"
log "base time END:   $END"
log "replay time START: $REPLAY_START"
log "replay time END:   $REPLAY_END"

log -n "base:   "
ftdc stats $DBPATH/diagnostic.data/* --start="$START" --end="$END" --out="tmp.base.stat.json"
log -n "replay: "
ftdc stats $DBPATH/diagnostic.data/* --start="$REPLAY_START" --end="$REPLAY_END" --out="tmp.play.stat.json"

set +e
ftdc compare $EXPLICIT tmp.base.stat.json tmp.play.stat.json
CODE=$?

if [ ! $KEEP ]; then
  rm tmp.playback
  rm tmp.base.stat.json
  rm tmp.play.stat.json
  rm tmp.report
fi
exit $CODE
