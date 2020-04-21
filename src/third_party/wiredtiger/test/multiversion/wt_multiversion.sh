#!/bin/bash

last_stable=4.2
last_stable_dir=wiredtiger_${last_stable}/
last_stable_branch=mongodb-${last_stable}

function setup_last_stable {
    git clone git@github.com:wiredtiger/wiredtiger.git ${last_stable_dir}
    cd ${last_stable_dir}/build_posix/ || exit
    git checkout $last_stable_branch || exit 1
    bash reconf
    ../configure --enable-python --enable-diagnostic
    make -j 10
    # Back to multiversion/ in "latest" repo.
    cd ../../ || exit
}

function run_check {
    echo + "$@"
    "$@" || exit 1
}

# Clone and build v4.2 if it doesn't already exist.
if [ ! -d $last_stable_dir ]; then
    setup_last_stable
fi

latest_workgen=../../bench/workgen/runner/multiversion.py
last_stable_workgen=${last_stable_dir}/bench/workgen/runner/multiversion.py

# Copy the workload into the v4.2 tree.
cp $latest_workgen $last_stable_workgen

run_check $latest_workgen --release 4.4
run_check $latest_workgen --keep --release 4.4
run_check $last_stable_workgen --keep --release 4.2
run_check $latest_workgen --keep --release 4.4

echo Success.
exit 0
