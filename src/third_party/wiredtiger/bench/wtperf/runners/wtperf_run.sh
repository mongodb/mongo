#!/bin/bash

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
# config to run and the number of runs.
#
if test "$#" -lt "2"; then
	echo "Must specify wtperf test to run and number of runs"
	exit 1
fi
wttest=$1
shift # Consume this arg
runmax=$1
shift # Consume this arg
# Jenkins removes the quotes from the passed in arg so deal with an arbitrary
# number of arguments
wtarg=""
create=1
while [[ $# -gt 0 ]] ; do
	if test "$1" == "NOCREATE"; then
		create=0
	else
		wtarg+=" $1"
	fi
	shift # Consume this arg
done

home=./WT_TEST
outfile=./wtperf.out
rm -f $outfile
echo "Parsed $# args: test: $wttest runmax: $runmax args: $wtarg" >> $outfile

# Each of these has an entry for each op in ops below.
avg=(0 0 0 0 0)
max=(0 0 0 0 0)
min=(0 0 0 0 0)
sum=(0 0 0 0 0)
# Load needs floating point and bc, handle separately.
loadindex=6
avg[$loadindex]=0
max[$loadindex]=0
min[$loadindex]=0
sum[$loadindex]=0
ops=(insert modify read truncate update)
outp=("Insert count:" "Modify count:" "Read count:" "Truncate count:" "Update count:"  )
outp[$loadindex]="Load time:"

# getval min/max val cur
# Returns the minimum or maximum of val and cur.
# min == 0, max == 1.
getval()
{
	max="$1"
	val="$2"
	cur="$3"
	ret=$cur
	echo "getval: max $max val $val cur $cur" >> $outfile
	if test "$max" -eq "1"; then
		if test "$val" -gt "$cur"; then
			ret=$val
		fi
	elif test "$val" -lt "$cur"; then
			ret=$val
	fi
	echo "$ret"
}

isstable()
{
	min="$1"
	max="$2"
	tmp=`echo "scale=3; $min * 1.03" | bc`
	if (($(bc <<< "$tmp < $max") )); then
		ret=0
	else
		ret=1
	fi
	echo "$ret"
}

getmin=0
getmax=1
run=1
while test "$run" -le "$runmax"; do
	if test "$create" -eq "1"; then
		rm -rf $home
		mkdir $home
	fi
	LD_PRELOAD=/usr/local/lib/libtcmalloc.so LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib ./wtperf -O $wttest $wtarg
	if test "$?" -ne "0"; then
		exit 1
	fi

	# Copy the artifacts from the run
	backup_dir=${home}_$(basename $wttest)_${run}_$(date +"%s")
	rsync -r -m --include="*Stat*" --include="CONFIG.wtperf" --include="*monitor" --include="latency*" --include="test.stat" --exclude="*" $home/ $backup_dir

	# Load is always using floating point, so handle separately
	l=`grep "^Load time:" ./WT_TEST/test.stat`
	if test "$?" -eq "0"; then
		load=`echo $l | cut -d ' ' -f 3`
	else
		load=0
	fi
	cur[$loadindex]=$load
	sum[$loadindex]=`echo "${sum[$loadindex]} + $load" | bc`
	echo "cur ${cur[$loadindex]} sum ${sum[$loadindex]}" >> $outfile
	for i in ${!ops[*]}; do
		l=`grep "Executed.*${ops[$i]} operations" ./WT_TEST/test.stat`
		if test "$?" -eq "0"; then
			n=`echo $l | cut -d ' ' -f 2`
		else
			n=0
		fi
		cur[$i]=$n
		sum[$i]=`expr $n + ${sum[$i]}`
	done
	#
	# Keep running track of min and max for each operation type.
	#
	if test "$run" -eq "1"; then
		for i in ${!cur[*]}; do
			min[$i]=${cur[$i]}
			max[$i]=${cur[$i]}
		done
	else
		for i in ${!cur[*]}; do
			if test "$i" -eq "$loadindex"; then
				if (($(bc <<< "${cur[$i]} < ${min[$i]}") )); then
					min[$i]=${cur[$i]}
				fi
				if (($(bc <<< "${cur[$i]} > ${max[$i]}") )); then
					max[$i]=${cur[$i]}
				fi
			else
				min[$i]=$(getval $getmin ${cur[$i]} ${min[$i]})
				max[$i]=$(getval $getmax ${cur[$i]} ${max[$i]})
			fi
		done
	fi
	#
	# After 3 runs see if this is a very stable test.  If so, we
	# can skip the last 2 runs and just use these values.  We
	# define "very stable" to be that the min and max are within
	# 3% of each other.
	if test "$run" -eq "3"; then
		# Only if all values are stable, we can break.
		unstable=0
		for i in ${!min[*]}; do
			stable=$(isstable ${min[$i]} ${max[$i]})
			if test "$stable" -eq "0"; then
				unstable=1
				break
			fi
		done
		if test "$unstable" -eq "0"; then
			break
		fi
	fi
	run=`expr $run + 1`
done

skipminmax=0
if test "$runmax" -le "2"; then
	numruns=$(getval $getmin $run $runmax)
	skipminmax=1
elif test "$run" -le "$runmax"; then
	numruns=`expr $run - 2`
else
	numruns=`expr $runmax - 2`
fi
if test "$numruns" -eq "0"; then
	$numruns=1
fi
#
# The sum contains all runs.  Subtract out the min/max values.
# Average the remaining and write it out to the file.
#
for i in ${!min[*]}; do
	if test "$i" -eq "$loadindex"; then
		if test "$skipminmax" -eq "0"; then
			s=`echo "scale=3; ${sum[$i]} - ${min[$i]} - ${max[$i]}" | bc`
		else
			s=${sum[$i]}
		fi
		avg[$i]=`echo "scale=3; $s / $numruns" | bc`
	else
		if test "$skipminmax" -eq "0"; then
			s=`expr ${sum[$i]} - ${min[$i]} - ${max[$i]}`
		else
			s=${sum[$i]}
		fi
		avg[$i]=`expr $s / $numruns`
	fi
done
for i in ${!outp[*]}; do
	echo "${outp[$i]} ${avg[$i]}" >> $outfile
done
exit 0
