#!/bin/bash

live_restore_binary_path="./test/cppsuite/test_live_restore"

function run_test() {
    $live_restore_binary_path $1
    exit=$?

    if [ $exit -ne 0 ]; then
        echo "Test failed!"
        echo "Command: " $1
        echo "Exit code: " $exit
        exit $exit
    fi
}
