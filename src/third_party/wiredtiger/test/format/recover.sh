#! /bin/sh

# Ulimit: don't drop core.
ulimit -c 0

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
rundir2=RUNDIR.SAVE
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

	rm -rf $rundir2
	$tcmd $config -q abort=1 logging=1 timer=$timer

	# Save a copy of the database directory exactly as it was at the crash.
	cp -rp RUNDIR $rundir2

	#
	# Everything is a table unless explicitly a file.
	#
	isfile=`grep data_source RUNDIR/CONFIG | grep -c file || exit 0`
	if test "$isfile" -ne 0; then
		uri="file:wt"
	else
		uri="table:wt"
	fi

	# We know we aborted, so force recovery to run.
	$wtcmd -R -h RUNDIR verify $uri || exit 1
done
