#! /usr/bin/env bash
#
# Cycle through a list of test/format configurations that failed previously,
# and run format test against each of those configurations to capture issues.
#
# Note - The failed configs must not require restart (such as abort)

set -e
# Switch to the Git repo toplevel directory
cd $(git rev-parse --show-toplevel)
# Walk into the test/format directory
cd cmake_build/test/format
set +e

# Check the existence of 't' binary
if [ ! -x "t" ]; then
	echo "'t' binary does not exist, exiting ..."
	exit 1
fi

success=0
failure=0
running=0
parallel_jobs=8
cleanup=0
PID=""
declare -a PID_LIST

usage() {
	echo "usage: $0 "
	echo "    -j <num_of_jobs>  number of jobs to execute in parallel (defaults to 8)"

	exit 1
}

# Wait for other format runs.
wait_for_process()
{
	while true ; do
		for process in ${PID_LIST[@]};do
			ps $process > /dev/null; ps_exit_status=$?
			if [ ${ps_exit_status} -eq "1" ] ; then
				# The process is completed so remove the process id from the list of processes.
				# Need to use a loop to prevent partial regex matches.
				unset NEW_PID_LIST
				declare -a NEW_PID_LIST
				for target in ${PID_LIST[@]};do
					if [ $target -ne $process ]; then
						NEW_PID_LIST+=("$target")
					fi
				done
				PID_LIST=("${NEW_PID_LIST[@]}")

				# Wait for the process to get the exit status.
				wait $process
				exit_status=$?

				let "running--"

				# Grep for the exact process id in the temp file.
				config_name=`grep -E -w "${process}" $tmp_file | awk -F ":" '{print $2}' | rev | awk -F "/" '{print $1}' | rev`
				if [ $exit_status -ne "0" ]; then
					let "failure++"
					[ -f WT_TEST_${config_name}/CONFIG ] && cat WT_TEST_${config_name}/CONFIG
				else
					let "success++"
					# Remove database files of successful jobs.
					[ -d WT_TEST_${config_name} ] && rm -rf WT_TEST_${config_name}
				fi

				echo "Exit status of pid ${process} and config ${config_name} is ${exit_status}"
				# Continue checking other runnung process status before exiting the for loop.
				continue
			fi
		done

		if [ ${cleanup} -ne 0 ]; then
			# Cleanup, wait for all the remaining processes to complete.
			[ ${#PID_LIST[@]} -eq 0 ] && break
		elif [ $running -lt $parallel_jobs ]; then
			# Break and invoke a new process if any of the process have completed.
			break
		fi

		# Sleep for 2 seconds before iterating the list of running processes.
		sleep 2
	done
}

while true ; do
	case "$1" in
	-j)
		parallel_jobs="$2"
		[[ "$parallel_jobs" =~ ^[1-9][0-9]*$ ]] ||
			fatal_msg "-j option argument must be a non-zero integer"
		shift ; shift ;;
	-*)
		usage ;;
	*)
		break ;;
	esac
done

tmp_file="format_list.txt"
touch $tmp_file
# Cycle through format CONFIGs recorded under the "failure_configs" directory
for config in $(find ../../../test/format/failure_configs/ -name "CONFIG.*" | sort)
do
	echo -e "\nTesting CONFIG $config ...\n"
	basename_config=$(basename $config)
	./t -1 -c $config -h WT_TEST_$basename_config &
	let "running++"

	PID="$!"
	PID_LIST+=("$PID")

	echo "${PID}:$config" >> $tmp_file

	if [ ${running} -ge ${parallel_jobs} ]; then
		wait_for_process
	fi
done

# Cleanup, wait for all the remaining processes to complete.
cleanup=1
wait_for_process

echo -e "\nSummary of '$(basename $0)': $success successful CONFIG(s), $failure failed CONFIG(s)\n"

[[ $failure -ne 0 ]] && exit 1
exit 0
