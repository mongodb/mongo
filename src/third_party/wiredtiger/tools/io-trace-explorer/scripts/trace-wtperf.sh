#!/bin/bash
#
# Copyright (c) 2014-present MongoDB, Inc.
# Copyright (c) 2008-2014 WiredTiger, Inc.
#    All rights reserved.
#
# See the file LICENSE for redistribution information.
#

#
# This tool runs wtperf and captures the output.
#

WT_DIR="`git rev-parse --show-toplevel`"
WT_PERF="build/bench/wtperf/wtperf"


# Parse the command-line arguments.

DEFAULT_WORKLOAD_DIR=WT_PERF
FORMAT=
OUTPUT_DIR="`pwd`"
OUTPUT_TAG=
WORKLOAD=
WORKLOAD_DEVICE=
WORKLOAD_DIR=
WORKLOAD_FS=

function usage() {
    echo "Usage: $0 [OPTIONS] WORKLOAD"
    echo "  WORKLOAD can be a file or one of the predefined workloads"
    echo
    echo "Options:"
    echo "  -D DEVICE  set the workload device"
    echo "  -d DIR     set the workload home (must be a mountpoint if -F is used)"
    echo "  -F FS      format the device with the specified file system"
    echo "  -h         print this help"
    echo "  -O DIR     set the output directory for traces"
    echo "  -T TAG     set the tag (the prefix) for the output files"
}

while getopts 'D:d:F:hT:O:' OPT; do
    case "$OPT" in
        D)
            WORKLOAD_DEVICE="$OPTARG"
            ;;
        d)
            WORKLOAD_DIR="$OPTARG"
            ;;
        F)
            FORMAT=1
            WORKLOAD_FS="$OPTARG"
            ;;
        h)
            usage
            exit
            ;;
        O)
            OUTPUT_DIR="$OPTARG"
            ;;
        T)
            OUTPUT_TAG="$OPTARG"
            ;;
        \?)
            echo
            usage
            exit 1
            ;;
    esac
done
shift $((OPTIND-1))

if [ -z $1 ] || [ ! -z $2 ]; then
    echo "$0: The workload is not specified." >&2
    echo
    usage
    exit 1
fi

WORKLOAD_FILE="$1"


# Check that all relevant tools are installed.

exists() {
    command -v "$*" >/dev/null 2>&1
    return $?
}

if ! exists $WT_DIR/$WT_PERF; then
    echo "$0: Cannot find wtperf. Is it compiled?" >&2
    exit 1
fi

for T in blkparse blktrace iowatcher; do
    if ! exists $T; then
        echo "$0: The following tool is not installed: $T" >&2
        exit 1
    fi
done

if [ ! -z $WORKLOAD_FS ]; then
    for T in mkfs.${WORKLOAD_FS} wipefs; do
        if ! exists $T; then
            echo "$0: The following tool is not installed: $T" >&2
            exit 1
        fi
    done
fi



#
# Prepare the test.
#

# Check that the workload file exists.

if [ `echo $WORKLOAD_FILE | grep -c -E '^[a-z0-9_-]+$'` == 1 ]; then
    WORKLOAD_FILE=$WT_DIR/bench/wtperf/runners/${WORKLOAD_FILE}.wtperf
fi
if [ ! -f $WORKLOAD_FILE ]; then
    echo "$0: File not found: $WORKLOAD_FILE"
    exit 1
fi


# Get the short name for the workload.

WORKLOAD=`basename "$WORKLOAD_FILE" | sed 's/\.wtperf$//'`


# If we are formatting the file system, check that the user has specified either the workload
# directory or a device. We cannot use any defaults for safety reasons here.

if [ $FORMAT ]; then
    if [ -z $WORKLOAD_DIR ] && [ -z $WORKLOAD_DEVICE ]; then
        echo "$0: The workload device or directory must be specified when the -F option is used" >&2
        exit 1
    fi

    # If the workload device is specified, but the workload dir is not, then get it from the
    # mount point. In that case, we also require that the workload dir is the mount point.
    if [ -z $WORKLOAD_DEVICE ]; then
        if ! findmnt -n -o SOURCE -M "$WORKLOAD_DIR" > /dev/null; then
            echo "$0: The workload directory is not a mountpoint" >&2
            exit 1
        fi
        WORKLOAD_MOUNT="$WORKLOAD_DIR"
        WORKLOAD_DEVICE=`findmnt -n -o SOURCE -T "$WORKLOAD_DIR"`
    fi

    # If the workload device is specified, we'll use the directory as the new mountpopint.
fi


# If the workload directory is not specified, use the default.

if [ -z $WORKLOAD_DIR ]; then
    WORKLOAD_DIR=$DEFAULT_WORKLOAD_DIR
fi


# The workload directory must exist beyond this point.

mkdir -p "$WORKLOAD_DIR" || exit 1


# If we are not formatting and the device is specified, make sure it's the right one.

if [ -z $FORMAT ] && [ ! -z $WORKLOAD_DEVICE ]; then
    if [ $WORKLOAD_DEVICE != `findmnt -n -o SOURCE -T "$WORKLOAD_DIR"` ]; then
        echo "$0: The workload device does not match the workload directory."
        exit 1
    fi
fi


# Get the device if it is not specified. If it is, check that it is really a block device.

if [ -z $WORKLOAD_DEVICE ]; then
    WORKLOAD_DEVICE=`findmnt -n -o SOURCE -T "$WORKLOAD_DIR"`
else
    if [ `stat $WORKLOAD_DEVICE | grep -c "block special file"` != 1 ]; then
        echo "$0: Not a block device: $WORKLOAD_DEVICE" >&2
        exit 1
    fi
fi


# Set the output tag (the prefix for output files).

if [ -z $OUTPUT_TAG ]; then
    OUTPUT_TAG="${WORKLOAD}"
    if [ ! -z $WORKLOAD_FS ]; then
        OUTPUT_TAG="${OUTPUT_TAG}-${WORKLOAD_FS}"
    fi
fi


# Check that the output directory exists and that it does not have output files with our tag.

if [ ! -d $OUTPUT_DIR ]; then
    echo "$0: The output directory does not exists: ${OUTPUT_DIR}" >&2
    exit 1
fi

if [ `ls -1 $OUTPUT_DIR | T="$OUTPUT_TAG" awk 'index($0, ENVIRON["T"]) == 1' | wc -l` != 0 ]; then
    echo "$0: The output directory \"$OUTPUT_DIR\" contains files that start with \"$OUTPUT_TAG\""
    exit 1
fi


# Make sure the output directory is not on the same device as the workload.

if [ $WORKLOAD_DEVICE = `findmnt -n -o SOURCE -T "$OUTPUT_DIR" | head -n 1` ]; then
    echo "$0: The output directory cannot be on the same device as the workload." >&2
    exit 1
fi



# Get the mountpoint.

if [ $FORMAT ]; then
    # We'll actually create a directory within the mountpoint
    WORKLOAD_MOUNT="$WORKLOAD_DIR"
    WORKLOAD_DIR="$WORKLOAD_MOUNT/wt"
else
    WORKLOAD_MOUNT=`findmnt -n -o TARGET -S "$WORKLOAD_DEVICE" | head -n 1`
fi


# Print the summary

echo "WiredTiger directory: $WT_DIR"
echo "Workload device     : $WORKLOAD_DEVICE"
echo "Workload directory  : $WORKLOAD_DIR"
echo "Workload file system: $WORKLOAD_FS"
echo "Workload mountpoint : $WORKLOAD_MOUNT"
echo "Workload script     : $WORKLOAD_FILE"
echo "Output directory    : $OUTPUT_DIR"
echo "Output tag          : $OUTPUT_TAG"
echo

if [ $FORMAT ]; then
    echo "$0: $WORKLOAD_DEVICE will be formatted."
    echo
fi


#
# Initialize the test.
#

# Format

if [ $FORMAT ]; then
    echo "Formatting ${WORKLOAD_DEVICE} to ${WORKLOAD_FS}."
    sudo umount $WORKLOAD_DEVICE || true
    sudo wipefs -a $WORKLOAD_DEVICE || exit $?
    sudo mkfs.$WORKLOAD_FS $WORKLOAD_DEVICE || exit $?
    sudo mount $WORKLOAD_DEVICE $WORKLOAD_MOUNT || exit $?
    sudo chown $USER:$USER $WORKLOAD_MOUNT || exit $?
    echo
else
    rm -rf $WORKLOAD_DIR || exit $?
fi

mkdir -p $WORKLOAD_DIR || exit $?


#
# Run the workload and capture the traces.
#

echo "$0: Running ${WORKLOAD}."

# We will be changing directories, so make sure we have full paths

OUTPUT_DIR=`realpath $OUTPUT_DIR`
WORKLOAD_DIR=`realpath $WORKLOAD_DIR`
WORKLOAD_FILE=`realpath $WORKLOAD_FILE`

# Start the trace

(cd "$OUTPUT_DIR" && sudo blktrace -d $WORKLOAD_DEVICE -o $OUTPUT_TAG) &

# Start the workload

(cd "$WT_DIR" && export WIREDTIGER_CONFIG="verbose=[read:2,write:2]" && \
    $WT_PERF -O $WORKLOAD_FILE -h $WORKLOAD_DIR \
        > ${OUTPUT_DIR}/${OUTPUT_TAG}---stdout.txt ; \
    sleep 10 ; sudo killall -INT blktrace) || true

echo "$0: The workload finished."


# Wait and parse

sleep 10

(cd "$OUTPUT_DIR" && blkparse -t -i ${OUTPUT_TAG} > ${OUTPUT_TAG}---device.txt)
(cd "$OUTPUT_DIR" && iowatcher -t ${OUTPUT_TAG} -o ${OUTPUT_TAG}---summary.svg)

echo
echo '********** DONE *********'
