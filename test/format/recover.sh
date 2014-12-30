#! /bin/sh

# Timer: how many minutes format runs before aborting.
timer=2

# Runs: set to 0 to run infinitely.
runs=1000
if test "$#" -gt "0"; then
	runs=$1
fi

# Config: additional test/format configuration
config=

# Assumes we're running in build_*/test/format directory.
tcmd=./t
wtcmd=../../wt
count=0
while true; do
	count=`expr $count + 1`
	if test $runs -eq 0; then
		echo "recovery test: $count"
	else
		if test $count -gt $runs; then
			exit 0
		fi
		echo "recovery test: $count of $runs"
	fi

	$tcmd $config -q abort=1 logging=1 timer=$timer

	uri='file:wt'
	if `$wtcmd -h RUNDIR list | egrep table > /dev/null`; then
		uri='table:wt'
	fi
	$wtcmd -h RUNDIR verify $uri || exit 1
done
