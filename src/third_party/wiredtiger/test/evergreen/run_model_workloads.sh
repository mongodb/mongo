#! /usr/bin/env bash
#
# Run the test/model workloads that failed previously.

# Switch to the model-test directory.
cd $(git rev-parse --show-toplevel)/cmake_build/test/model/tools || exit 1

# Check the existence of the 'model_test' binary.
if [ ! -x model_test ]; then
    echo "'model_test' binary does not exist, exiting ..."
    exit 1
fi

# Run all workloads.
SUCCESS=0
FAILURE=0
for W in $(find ../../../../test/model/workloads -name "*.workload" | sort)
do
    BASENAME_WORKLOAD=$(basename ${W%.workload})
    echo
    echo "Testing workload $BASENAME_WORKLOAD"

    # FIXME-WT-13232 The WT-12539 workload can be re-enabled once we've confirmed
    # how to handle prepare conflicts on keys adjacent to the truncation range.
    if [[ "$BASENAME_WORKLOAD" = "WT-12539" ]]; then
        echo "Skipping workload"
        continue
    fi

    ./model_test -R -h "WT_TEST_$BASENAME_WORKLOAD" -w "$W"
    RESULT="$?"

    if [ $RESULT == 0 ]; then
        let "SUCCESS++"
    else
        let "FAILURE++"
    fi
done

# Print the summary and finish.
echo
echo "Summary: $SUCCESS successful, $FAILURE failed workloads(s)"
[[ $FAILURE -ne 0 ]] && exit 1
exit 0
