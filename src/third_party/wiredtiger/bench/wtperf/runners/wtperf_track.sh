#!/bin/bash

# wtperf_track.sh - track performance regression trends on the Jenkins platform.
#
# This is run after a performance build, with a count or time value.
#    wtperf_track.sh -t [ -p percent ] name time_value
#    wtperf_track.sh -c [ -p percent ] name count_value
#
# The values are kept in a .csv file (see $TRACK_FILE) stored under
# the userContent directory (see $STATE_DIR), which is served as
# visible files by Jenkins.  The .csv contains fields: buildid,
# curtime, value, loadavg, diskavg.
#
# Since our Jenkins hosts have some variability to their load level,
# we allow some 'resiliance' in the regression checking.  To this end,
# we calculate 3 values:
#
#    v3: The 'best' of the most recent 3 values, where 'best' is
#       lowest for a time value and highest for a count.
#
#    v20: The average of the best 10 of the last 20 values.
#
#    v100: The average of the best 50 of the last 100 values.
#
# If v3 is more then p% worse than v20, or if v20 is more than p% worse
# than v100, we issue a warning.  The default value of p is 5, and
# is set with the -p option.
#
# We expect that the Jenkins job is configured to capture the WARNING
# output, in order to send mail or mark the build as unstable.

JENKINS_HOME=${JENKINS_HOME:?"Must be run on the Jenkins platform"}
JOB_NAME=${JOB_NAME:?"Must be run on the Jenkins platform"}
BUILD_ID=${BUILD_ID:?"Must be run on the Jenkins platform"}

STATE_DIR="/home/jenkins/wtperf_track"
TYPE=unknown  # set to 'time' or 'count' depending on -t/-c option
NAME=         # the command line name
VALUE=        # the command line value

Usage()
{
    cat >&2 <<EOF
Usage: wtperf_track.sh -t [ options ] name time_value
       wtperf_track.sh -c [ options ] name count_value

name is the name of this metric (e.g. "load_time")
value is an integer or float value

Options:
  -t:     value is a time value, lower numbers are better
  -c:     value is a count value, higher numbers are better
  -p pct: require short term avgs to be witnin 'pct' percent of long term avgs

One of the -t and -c options is required.
EOF
}

# GetValues n filename
# gets the last n values from a .csv file
GetValues()
{
    cut -f 3 -d , "$2" | sed -e '1d' -e 's/"//g' | tail -"$1"
}

# MinValues n direction
# Given a list of values on input,
# removes all but n smallest (if direction is >0) or
# n largest if (direction is <0)
MinValues()
{
    rflag=-r
    if [ "$2" -gt 0 ]; then
        rflag=
    fi
    sort -g $rflag | tail -$1
}

# AvgValues
# Given a list of values on input,
# returns the average.
AvgValues()
{
    VALUES=$(tr '\n' ' ' | sed -e 's/^ *//' -e 's/ *$//' -e 's/  */ /')
    n=$(echo "$VALUES" | wc -w)
    echo "$VALUES" | sed -e 's/ /+/g' -e 's/^/scale=3;(/' -e "s:\$:)/$n:"| bc
}

# CheckValues sval lval direction pct desc
# Given two values 'sval', 'lval', make sure that sval is no more
# than 'pct' percent less that lval (if direction is >0) or that
# sval is no more than 'ct' percent greater than lval (if direction <0)
# Returns 0 for normal, 1 for out of range
CheckValues()
{
    sval="$1"
    lval="$2"
    direction="$3"
    pct="$4"
    desc="$5"
    if [ "$direction" -gt 0 ]; then
        expr="($sval * (1.00 + $pct / 100.00)) - $lval"
    else
        expr="($lval * (1.00 + $pct / 100.00)) - $sval"
    fi
    result=$(echo "scale=3; $expr" | bc)
    # bc error?
    if [ $? != 0 ]; then
        return 1
    fi
    if [ "$(echo $result | grep '^-')" != '' ]; then
        echo "$desc: WARNING: $type $sval not within $pct% of $lval (curval=$VALUE)" >&2
        return 1
    fi
    return 0
}

GetCpuLoadAverage()
{
    uptime | sed -e 's/.*: *//' -e 's/ .*//'
}

GetDiskLoadAverage()
{
    DEVICE=$(df "$1" | grep /dev | head -1 | sed -e 's:.*\(/dev/[^	 ]*\).*:\1/:')
    case `uname -s` in
        *Linux* )
            # iostat -d $DEVICE | grep -v Device: | head -1 | sed -e 's/.* //'
	    echo '0.0'
            ;;
        * )
            echo '0.0'
            ;;
    esac
}

direction=0
pct=5
while getopts tcp: OPT; do
    case "$OPT" in
        t)
            direction=-1
            type=time
            ;;
        c)
            direction=1
            type=count
            ;;
        p)
            pct=$OPTARG
            ;;
        *)
            # getopts issues an error message
            Usage
            exit 1
            ;;
    esac
done

shift $((OPTIND-1))
if [ "$#" != 2 ]; then
    echo "Missing name/value" >&2
    Usage
    exit 1
fi
NAME="$1"
VALUE="$2"

if [ "$direction" = 0 ]; then
    echo "Missing -t or -c option" >&2
    Usage
    exit 1
fi

TRACK_FILE="${STATE_DIR}/${JOB_NAME}.${NAME}.csv"

mkdir -p "${STATE_DIR}" || exit 1
if [ ! -f "${TRACK_FILE}" ]; then
    echo '"buildid","time","value","loadavg","diskavg"' \
        > ${TRACK_FILE} || exit 1
fi
TIME=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
LOAD_AVG=$(GetCpuLoadAverage)
DISK_AVG=$(GetDiskLoadAverage .)
echo "${BUILD_ID},${TIME},${VALUE},${LOAD_AVG},${DISK_AVG}" \
    >> ${TRACK_FILE} || exit 1

v3=$(GetValues 3 ${TRACK_FILE} | MinValues 1 $direction)
v20=$(GetValues 20 ${TRACK_FILE} | MinValues 10 $direction | AvgValues)
v100=$(GetValues 100 ${TRACK_FILE} | MinValues 50 $direction | AvgValues)

v3=${v3:?"Internal error: v3 not set"}
v20=${v20:?"Internal error: v20 not set"}
v100=${v100:?"Internal error: v100 not set"}

ecode=0
prefix="$JOB_NAME: build #$BUILD_ID"
echo "$prefix: curval=$VALUE v3=$v3 v20=$v20 v100=$v100 pct=$pct"
CheckValues $v3 $v20 $direction $pct "$prefix: short term trend" || ecode=1
CheckValues $v20 $v100 $direction $pct "$prefix: long term trend" || ecode=1
# Rather than a failure exit for a trend out of range,
# Jenkins can capture the WARNING output, send email and mark
# a build as unstable.
#exit $ecode
