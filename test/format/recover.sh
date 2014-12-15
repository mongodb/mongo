#! /bin/sh

# Timer: how many minutes format runs before aborting.
timer=2

# Runs: set to 0 to run infinitely.
runs=1000

# Config: additional test/format configuration
config=

count=0
while true; do
	count=`expr $count + 1`
	if test $runs -ne 0 -a $count -gt $runs; then
		exit 0
	fi
	echo "recovery test: $count of $runs"

	./t $config -q abort=1 timer=$timer

	uri='file:wt'
	if `wt -h RUNDIR list | egrep table > /dev/null`; then
		uri='table:wt'
	fi
	wt -h RUNDIR verify $uri || exit 1
done
