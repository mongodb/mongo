#! /bin/sh

set -e

# the purpose of this test is to ensure we use monotonic clock instead of
# realtime clock in our code. we had the instances where WT is hanging when
# system clock shifts (for eg: due to NTP servers). this test calculates
# the execution time of a test(test_rwlock), shifts the clock -vely by that
# time period and reexecutes the test. if the difference in the two execution
# times is less than 20% test is considered passed. 20% is selected, based on
# assumption that other factors of the environment will influence the execution
# time by less than 20%.

EXIT_SUCCESS=0
EXIT_FAILURE=1

export DONT_FAKE_MONOTONIC=1
RUN_OS=$(uname -s)

# linux we run with cpu affinity, to control the execution time
# if we don't control the execution time this test is not effective
CPU_SET=0-1
echo "test read write lock for time shifting using libfaketime"


# check for program arguements, if not present, print usage
if [ -z $1 ]
then
    echo "fail : this test needs libfaketime library with path"
    echo "Usage :"
    echo "       " $0 " <libpath> [cpuset] "
    echo "         libpath : path to libfaketime library"
    echo "         cpuset  : set of cpu's to be used for taskset on linux"
    echo "                 : default is 0-1 "
    exit $EXIT_FAILURE
fi

# check for the existence of dependent library
if [ ! -r $1 ]
then
    echo "fail : $1 , libfaketime library is not readable"
    exit $EXIT_FAILURE
fi

# Locate Wiredtiger home directory.
# If RW_LOCK_FILE isn't set, default to using the build directory this script resides under
# under. Our CMake build will sync a copy of this script to the build directory the 'test_rwlock'
# binary is created under. Note this assumes we are executing a copy of the script that lives under
# the build directory. Otherwise passing the binary path is required.
: ${RW_LOCK_FILE:=$(dirname $0)/test_rwlock}
[ -x ${RW_LOCK_FILE} ] || {
    echo "fail: unable to locate rwlock test binary"
    exit 1
}

SEC1=`date +%s`
if [ "$RUN_OS" = "Darwin" ]
then
    $RW_LOCK_FILE
elif [ "$RUN_OS" = "Linux" ]
then
    if [ -z $2 ]
    then
        echo "default taskset value is 0-1"
    else
        CPU_SET=$2
    fi
    taskset -c $CPU_SET $RW_LOCK_FILE
else
    echo "not able to decide running OS, so exiting"
    exit $EXIT_FAILURE
fi

SEC2=`date +%s`
DIFF1=$((SEC2 - SEC1))

# preload libfaketime
if [ "$RUN_OS" = "Darwin" ]
then
    export DYLD_FORCE_FLAT_NAMESPACE=y
    export DYLD_INSERT_LIBRARIES=$1
    $RW_LOCK_FILE &
else
    LD_PRELOAD=$1 taskset -c $CPU_SET $RW_LOCK_FILE &
fi

# get pid of test run in background
PID=$!

sleep 5s
echo "-$DIFF1""s" >| ~/.faketimerc

wait $PID

#kept echo statement here so as not to loose in cluster of test msgs. 
echo "after sleeping for 5 seconds set ~/.faketimerc value as -ve $DIFF1 seconds"
rm ~/.faketimerc

if [ "$RUN_OS" = "Darwin" ]
then
    export DYLD_FORCE_FLAT_NAMESPACE=
    export DYLD_INSERT_LIBRARIES=
fi
SEC3=`date +%s`
DIFF2=$((SEC3 - SEC2))

PERC=$((((DIFF2 - DIFF1)*100)/DIFF1)) 
echo "execution time difference : $PERC %, less than 20% is ok"
echo "normal execution time : $DIFF1 seconds"
echo "fake time reduction by : $DIFF1 seconds"
echo "execution time with -ve time shift : $DIFF2 seconds"

if [ "$PERC" -le 20 ]
then
   echo "pass : execution time is affected $PERC % by -ve time shift"
   exit $EXIT_SUCCESS
else
   echo "fail : execution time is affected $PERC % by -ve time shift"
   exit $EXIT_FAILURE
fi
