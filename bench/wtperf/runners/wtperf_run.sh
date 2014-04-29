#!/bin/sh

# wtperf_run.sh - run wtperf regression tests on the Jenkins platform.
#
# The Jenkins machines show variability so we run this script to run
# each wtperf test several times.  We throw away the min and max
# number and average the remaining values.  That is the number we
# give to Jenkins for plotting.  We write these values to a
# test.average file in the current directory (which is 
# build_posix/bench/wtperf).
#
# This script should be invoked with the pathname of the wtperf test
# config to run.
#
if test "$#" -ne "1"; then
	echo "Must specify wtperf test to run"
	exit 1
fi
wttest=$1
home=./WT_TEST
outfile=./wtperf.out
rm -f $outfile
runmax=5
run=1
# First run the test N times accumulating the sum, min and max
# as we go along.
ins_max=0
ins_min=0
ins_sum=0
load_max=0
load_min=0
load_sum=0
rd_max=0
rd_min=0
rd_sum=0
upd_max=0
upd_min=0
upd_sum=0

# getval min/max val cur
# Returns the minimum or maximum of val and cur.
# min == 0, max == 1.
getval()
{
	max="$1"
	val="$2"
	cur="$3"
	ret=$cur
	if test "$max" -eq "1"; then
		if test "$val" -gt "$cur"; then
			ret=$val
		fi
	elif test "$val" -lt "$cur"; then
			ret=$val
	fi
	echo "$ret"
}

getmin=0
getmax=1
while test "$run" -le "$runmax"; do
	rm -rf $home
	mkdir $home
	LD_PRELOAD=/usr/lib64/libjemalloc.so.1 LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib ./wtperf -O $wttest
	if test "$?" -ne "0"; then
		exit 1
	fi
	load=`grep "^Load time:" ./WT_TEST/test.stat | cut -d ' ' -f 3`
	rd=`grep "Executed.*read operations" ./WT_TEST/test.stat | cut -d ' ' -f 2`
	ins=`grep "Executed.*insert operations" ./WT_TEST/test.stat | cut -d ' ' -f 2`
	upd=`grep "Executed.*update operations" ./WT_TEST/test.stat | cut -d ' ' -f 2`
	#
	# Test this value against the min and max values.
	# Load times are floating point.  Send those through bc.
	#
	load_sum=`echo "$load_sum + $load" | bc`
	rd_sum=`expr $rd_sum + $rd`
	ins_sum=`expr $ins_sum + $ins`
	upd_sum=`expr $upd_sum + $upd`
	#
	# Keep running track of min and max for each operation type.
	#
	if test "$run" -eq "1"; then
		load_min=$load
		load_max=$load
		rd_min=$rd
		rd_max=$rd
		ins_min=$ins
		ins_max=$ins
		upd_min=$upd
		upd_max=$upd
	else
		if (($(bc <<< "$load < $load_min") )); then
			load_min=$load
		fi
		if (($(bc <<< "$load > $load_max") )); then
			load_max=$load
		fi
		rd_min=$(getval $getmin $rd $rd_min)
		rd_max=$(getval $getmax $rd $rd_max)
		ins_min=$(getval $getmin $ins $ins_min)
		ins_max=$(getval $getmax $ins $ins_max)
		upd_min=$(getval $getmin $upd $upd_min)
		upd_max=$(getval $getmax $upd $upd_max)
	fi
	run=`expr $run + 1`
done

numruns=`expr $runmax - 2`
#
# The sum contains all runs.  Subtract out the min/max values.
# Average the remaining and write it out to the file.
#
load_sum=`echo "scale=3; $load_sum - $load_min - $load_max" | bc`
load_avg=`echo "scale=3; $load_sum / $numruns" | bc`
rd_sum=`expr $rd_sum - $rd_min - $rd_max`
rd_avg=`expr $rd_sum / $numruns`
ins_sum=`expr $ins_sum - $ins_min - $ins_max`
ins_avg=`expr $ins_sum / $numruns`
upd_sum=`expr $upd_sum - $upd_min - $upd_max`
upd_avg=`expr $upd_sum / $numruns`
echo "Load time: $load_avg" >> $outfile
echo "Read count: $rd_avg" >> $outfile
echo "Insert count: $ins_avg" >> $outfile
echo "Update count: $upd_avg" >> $outfile
exit 0
