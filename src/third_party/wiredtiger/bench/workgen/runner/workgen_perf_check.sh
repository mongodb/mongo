#! /bin/bash
#
# Checks warnings and exceptions raised by a given workgen test as well as lantencies of specific operations.
# This script is used in evergreen to assess performance using workgen.
#

usage () {
    cat << EOF
Usage: $0 test_name output threshold_1 threshold_2 threshold_3
Arguments:
    test_name    # Test to run
    output       # File output
    threshold_1  # Maximum allowed warnings from DROP operations.
    threshold_2  # Maximum time in seconds a DROP operation can take.
    threshold_3  # Maximum allowed warnings from CREATE/INSERT/UPDATE operations.
EOF
}

if [ $1 == "-h" ]; then
    usage
    exit
fi

if [ "$#" -ne 5 ]; then
    echo "Illegal number of parameters."
    usage
    echo FAILED
    exit 1
fi

if [ ! -f $1 ]; then
    echo "$1 does not exist."
    echo FAILED
    exit 1
fi

ERROR=0
TEST=$1
OUTPUT=$2
DROP_WARNINGS_THRESHOLD=$3
MAX_DROP_THRESHOLD=$4
OP_WARNINGS_THRESHOLD=$5

echo "python3 $TEST 2>&1 | tee $OUTPUT"
python3 $TEST 2>&1 | tee $OUTPUT

# Check exceptions
if grep -io "exception" $OUTPUT; then
    echo ERROR
    ERROR=1
fi

# Maximum number of DROP warnings
DROP_WARNINGS=$(grep -ic "cycling idle.*drop" $OUTPUT)
echo "Number of long drop operations: $DROP_WARNINGS"

# Maximum number of READ/INSERT/UPDATE warnings
OP_WARNINGS=$(grep -ic "max latency exceeded" $OUTPUT)
echo "Number of long read/insert/update operations: $OP_WARNINGS"

# Output the 5 worst DROP latencies
DROP_5=($(grep -i "cycling idle.*drop" $OUTPUT | awk '{print $9}' | sort -n | tail -5))
echo -n "Five worst drop operations (s): "
for ((i = 0; i < ${#DROP_5[@]}; ++i)); do
    echo -n "${DROP_5[$i]} "
done
echo

# Check if too many DROP operations took too long
if [[ $DROP_WARNINGS -ge $DROP_WARNINGS_THRESHOLD ]]; then
    echo "Too many long DROP operations: $DROP_WARNINGS (max allowed: $DROP_WARNINGS_THRESHOLD)"
    ERROR=1
fi

# Check if a DROP operation took too long
MAX_DROP=$(grep -i "cycling idle.*drop" $OUTPUT | awk '{print $9}' | sort -n | tail -1)
if [[ $MAX_DROP -ge $MAX_DROP_THRESHOLD ]]; then
    echo "A drop operation took too long: ${MAX_DROP}s (max allowed: ${MAX_DROP_THRESHOLD}s)"
    ERROR=1
fi

# Check if too many READ/INSERT/UPDATE operations took too long
if [[ $OP_WARNINGS -ge $OP_WARNINGS_THRESHOLD ]]; then
    echo "Too many long read/insert/update operations: $OP_WARNINGS (max allowed: $OP_WARNINGS_THRESHOLD)"
    ERROR=1
fi

if [[ $ERROR -ne 0 ]]; then
    echo FAILED
    exit 1
fi

echo SUCCESS
