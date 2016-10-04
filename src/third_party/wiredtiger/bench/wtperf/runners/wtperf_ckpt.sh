#!/bin/bash

# A script for running the wtperf benchmark to analyze the performance
# of checkpoint operations.

# General configuration settings:
BIN_DIR='.'
ROOT_DIR=`/bin/pwd`
SCRIPT_DIR=`dirname $0`
RUNTIME=900
REUSE=0
VERBOSE=0
WORKLOAD=0 # skip the populate phase.
PERF_BASE="-M"
OPTFILE=''
DEBUG=
GDB=${GDB:-gdb}

USAGE="Usage: `basename $0` [-hdRWsv] [-b binary dir] [-r root dir] [-O optfile]"

# Parse command line options.
while getopts b:dhO:RWr:sv OPT; do
    case "$OPT" in
        b)
            BIN_DIR=$OPTARG
            ;;
        d)
            export TERM=dtterm
            DEBUG="$GDB --args"
            ;;
        h)
            echo $USAGE
            exit 0
            ;;
        O)
            OPTFILE=-O$OPTARG
            PERF_BASE=""
            ;;
        R)
            REUSE=1
            ;;
        r)
            ROOT_DIR=$OPTARG
            ;;
        s)
            RUNTIME=20
	    PERF_BASE="-S"
            ;;
        v)
            VERBOSE=0
            ;;
        W)
            WORKLOAD=1
	    REUSE=1 # skip the populate phase.
            ;;
        \?)
            # getopts issues an error message
            echo $USAGE >&2
            exit 1
            ;;
    esac
done

# Configuration settings that may be altered by command line options
WTPERF=${BIN_DIR}/wtperf
if [ ! -x $WTPERF ]; then
	echo "Could not find or execute $WTPERF"
	exit 1
fi

DB_HOME="$ROOT_DIR/WT_TEST"
OUT_DIR="$ROOT_DIR/results"
SHARED_OPTS="${OPTFILE} ${PERF_BASE} -o read_threads=1,update_threads=1,report_interval=1,uri=\"table:test\" -o verbose=1 -h ${DB_HOME}"
CREATE_OPTS="$SHARED_OPTS -o run_time=0"
RUN_OPTS="$SHARED_OPTS -o run_time=$RUNTIME"
if [ $WORKLOAD -eq 0 ]; then
	RUN_OPTS="$RUN_OPTS -o create=false"
else
	RUN_OPTS="$RUN_OPTS -o icount=0"
fi

if [ $REUSE -eq 0 ]; then
	if [ $VERBOSE -ne 0 ]; then
		echo "Creating database and archiving it for reuse."
	fi
	rm -rf $DB_HOME && mkdir $DB_HOME
	$DEBUG $WTPERF $CREATE_OPTS || exit 1

	# Save the database so that it can be re-used by all runs.
	# I'd rather not hard code WT_TEST, but need to get the path right.
	rm -f $ROOT_DIR/WT_TEST.tgz
	tar zcf $ROOT_DIR/WT_TEST.tgz -C $ROOT_DIR WT_TEST
fi

rm -rf $OUT_DIR && mkdir $OUT_DIR

# Run the benchmarks..
# for ckpt in "" "-c 120"; do
for ckpt in "checkpoint_threads=1,checkpoint_interval=120"; do
	# for opts in "" "-C eviction_dirty_target=20"; do
	for opts in ""; do
		if [ $VERBOSE -ne 0 ]; then
			echo "Doing a run with:"
			echo "\t$WTPERF $RUN_OPTS $ckpt $opts"
		fi
		res_name="run_${ckpt},${opts}"
		res_name=`echo $res_name | tr '[:upper:][=\- ,]' '[:lower:]_'`
		if [ $WORKLOAD -eq 0 ]; then
			rm -rf $DB_HOME && tar zxf $ROOT_DIR/WT_TEST.tgz -C $ROOT_DIR
		else
			rm -rf $DB_HOME && mkdir $DB_HOME
		fi
		if [ "$DEBUG" = '' ]; then
			$WTPERF $RUN_OPTS -o "$ckpt" -o "$opts" &
			pid=$!
			t=0
			while kill -0 $pid 2> /dev/null; do
				echo "Time $t"
				pmp $pid
				sleep 1
				(( t++ ))
			done > $OUT_DIR/${res_name}.trace
		else
			$DEBUG $WTPERF $RUN_OPTS $ckpt $opts
		fi
		cp $DB_HOME/test.stat "$OUT_DIR/${res_name}.res"
	done
done

if [ $VERBOSE -ne 0 ]; then
	echo "Post processing result files."
fi
for f in ${OUT_DIR}/*res; do
	grep "^[0-9]* reads" ${f} | sed -e 's/ reads//' -e 's/ inserts//' -e 's/ updates in 1 secs//' > ${f}.out
	${SCRIPT_DIR}/get_ckpt.py < ${f} > ${f}.ckpt
done
