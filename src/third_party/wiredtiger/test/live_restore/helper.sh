#!/bin/bash

live_restore_binary_path="./test/cppsuite/test_live_restore"

function run_test() {
    $live_restore_binary_path $1
    exit=$?

    # Because the test may kill itself the exit code 137 is expected.
    if [ $exit -ne 0 ] && [ $exit -ne 137 ]; then
        echo "Test failed!"
        echo "Command: " $1
        echo "Exit code: " $exit
        exit $exit
    fi
}
