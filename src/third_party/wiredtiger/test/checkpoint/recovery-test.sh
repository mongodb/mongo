#!/usr/bin/env bash

set -x

usage () {
    cat << EOF
Usage: recovery_test.sh {config} {home directory}
EOF
}

if [ "$#" -ne 2 ]; then
    echo "Illegal number of parameters."
    usage
    exit 1
fi

config=$1
home=$2
backup=$home.backup
recovery=$home.recovery

#./t -t r -W 3 -D -X -n 100000 -k 100000 -C cache_size=100MB -h $home > $home.out 2>&1 &
./t ${config} -h ${home} > $home.out 2>&1 &
pid=$!

trap "kill -9 $pid" 0 1 2 3 13 15

# Wait for the test to start running
while ! grep -q "Finished a checkpoint" $home.out && kill -0 $pid ; do
	sleep 1
done

while kill -STOP $pid ; do
	rm -rf $backup $recovery ; mkdir $backup ; mkdir $recovery
	# Make sure all threads are stopped before copying files
	sleep 1
	cp $home/* $backup
	kill -CONT $pid
	cp $backup/* $recovery
	./t -t r -D -v -h $recovery || exit 1
done

exit 0
