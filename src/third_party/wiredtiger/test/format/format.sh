#! /bin/bash

[ -z $BASH_VERSION ] && {
	echo "$0 is a bash script: \$BASH_VERSION not set, exiting"
	exit 1
}

name=$(basename $0)

quit=0
force_quit=0
onintr()
{
	echo "$name: interrupted, cleaning up..."
	force_quit=1
}
trap 'onintr' 2

usage() {
	echo "usage: $0 [-aEFRSv] [-b format-binary] [-c config] [-D directory]"
	echo "    [-e env-var] [-h home] [-j parallel-jobs] [-n total-jobs] [-r live-record-binary]"
	echo "    [-t minutes] [format-configuration]"
	echo
	echo "    -a           add configuration for abort/recovery testing (defaults to off)"
	echo "    -b binary    format binary (defaults to "./t")"
	echo "    -c config    format configuration file (defaults to CONFIG.stress)"
	echo "    -D directory directory of format configuration files (named \"CONFIG.*\")"
	echo "    -E           skip known errors (defaults to off)"
	echo "    -e envvar    Environment variable setting (default to none)"
	echo "    -F           quit on first failure (defaults to off)"
	echo "    -h home      run directory (defaults to .)"
	echo "    -j parallel  jobs to execute in parallel (defaults to 8)"
	echo "    -n total     total jobs to execute (defaults to no limit)"
	echo "    -R           add configuration for randomized split stress (defaults to none)"
	echo "    -r binary    record with UndoDB binary (defaults to no recording)"
	echo "    -S           run smoke-test configurations (defaults to off)"
	echo "    -t minutes   minutes to run (defaults to no limit)"
	echo "    -v           verbose output (defaults to off)"
	echo "    --           separates $name arguments from additional format arguments"

	exit 1
}

# Smoke-tests.
smoke_base_1="runs.source=table rows=100000 threads=6 timer=4"
smoke_base_2="$smoke_base_1 leaf_page_max=9 internal_page_max=9"
smoke_list=(
	# Three access methods.
	"$smoke_base_1 file_type=row"
	"$smoke_base_1 file_type=fix"
	"$smoke_base_1 file_type=var"

	# Huffman value encoding.
	"$smoke_base_1 file_type=row huffman_value=1"
	"$smoke_base_1 file_type=var huffman_value=1"

	# LSM
    # Temporarily disabled: FIXME LSM
	# "$smoke_base_1 file_type=row runs.source=lsm"

	# Force the statistics server.
	"$smoke_base_1 file_type=row statistics_server=1"

	# Overflow testing.
	"$smoke_base_2 file_type=row key_min=256"
	"$smoke_base_2 file_type=row key_min=256 value_min=256"
	"$smoke_base_2 file_type=var value_min=256"
)

smoke_next=0
smoke_test=0

directory_next=0
directory_total=0

abort_test=0
build=""
config="CONFIG.stress"
first_failure=0
format_args=""
format_binary="./t"
home="."
live_record_binary=""
minutes=0
parallel_jobs=8
skip_errors=0
stress_split_test=0
total_jobs=0
verbose=0

while :; do
	case "$1" in
	-a)
		abort_test=1
		shift ;;
	-b)
		format_binary="$2"
		shift ; shift ;;
	-c)
		config="$2"
		shift ; shift ;;
	-D)
		# Format changes directories, get absolute paths to the CONFIG files.
		dir="$2"
		[[ "$dir" == /* ]] || dir="$PWD/$dir"
		directory_list=($dir/CONFIG.*)
		directory_total=${#directory_list[@]}
		[[ -f "${directory_list[0]}" ]] || {
		    echo "$name: no CONFIG files found in $2"
		    exit 1
		}
		shift ; shift ;;
	-E)
		skip_errors=1
		shift ;;
	-e)
		export "$2"
		shift ; shift ;;
	-F)
		first_failure=1
		shift ;;
	-h)
		home="$2"
		shift ; shift ;;
	-j)
		parallel_jobs="$2"
		[[ "$parallel_jobs" =~ ^[1-9][0-9]*$ ]] || {
			echo "$name: -j option argument must be a non-zero integer"
			exit 1
		}
		shift ; shift ;;
	-n)
		total_jobs="$2"
		[[ "$total_jobs" =~ ^[1-9][0-9]*$ ]] || {
			echo "$name: -n option argument must be an non-zero integer"
			exit 1
		}
		shift ; shift ;;
	-R)
		stress_split_test=1
		shift ;;
        -r)
		live_record_binary="$2"
		if [ ! $(command -v "$live_record_binary") ]; then
			echo "$name: -r option argument \"${live_record_binary}\" does not exist in path"
			echo "$name: usage and setup instructions can be found at: https://wiki.corp.mongodb.com/display/KERNEL/UndoDB+Usage"
			exit 1
		fi
		shift; shift ;;
	-S)
		smoke_test=1
		shift ;;
	-t)
		minutes="$2"
		[[ "$minutes" =~ ^[1-9][0-9]*$ ]] || {
			echo "$name: -t option argument must be a non-zero integer"
			exit 1
		}
		shift ; shift ;;
	-v)
		verbose=1
		shift ;;
	--)
		shift; break;;
	-*)
		usage ;;
	*)
		break ;;
	esac
done
format_args="$*"

verbose()
{
	[[ $verbose -ne 0 ]] && echo "$@"
}

verbose "$name: run starting at $(date)"

# Home is possibly relative to our current directory and we're about to change directories.
# Get an absolute path for home.
[[ -d "$home" ]] || {
	echo "$name: directory \"$home\" not found"
	exit 1
}
home=$(cd $home > /dev/null || exit 1 && echo $PWD)

# From the Bash FAQ, shuffle an array.
shuffle() {
    local i tmp size max rand

    size=${#directory_list[*]}
    for ((i=size-1; i>0; i--)); do
       # RANDOM % (i+1) is biased because of the limited range of $RANDOM
       # Compensate by using a range which is a multiple of the rand modulus.

       max=$(( 32768 / (i+1) * (i+1) ))
       while (( (rand=RANDOM) >= max )); do :; done
       rand=$(( rand % (i+1) ))
       tmp=${directory_list[i]} directory_list[i]=${directory_list[rand]} directory_list[rand]=$tmp
    done
}

# If we have a directory of CONFIGs, shuffle it. The directory has to be an absolute path so there
# is no additional path checking to do.
config_found=0
[[ $directory_total -ne 0 ]] && {
    shuffle
    config_found=1
}

# Config is possibly relative to our current directory and we're about to change directories.
# Get an absolute path for config if it's local.
[[ $config_found -eq 0 ]] && [[ -f "$config" ]] && {
    [[ "$config" == /* ]] || config="$PWD/$config"
    config_found=1
}

# Move to the format.sh directory (assumed to be in a WiredTiger build tree).
cd $(dirname $0) || exit 1

# If we haven't already found it, check for the config file (by default it's CONFIG.stress which
# lives in the same directory of the WiredTiger build tree as format.sh. We're about to change
# directories if we don't find the format binary here, get an absolute path for config if it's
# local.
[[ $config_found -eq 0 ]] && [[ -f "$config" ]] && {
    config="$PWD/$config"
    config_found=1
}

# Find the last part of format_binary, which is format binary file. Builds are normally in the
# WiredTiger source tree, in which case it's in the same directory as format.sh, else it's in
# the build_posix tree. If the build is in the build_posix tree, move there, we have to run in
# the directory where the format binary lives because the format binary "knows" the wt utility
# is two directory levels above it.
[[ -x ${format_binary##* } ]] || {
	build_posix_directory="../../build_posix/test/format"
	[[ ! -d $build_posix_directory ]] || cd $build_posix_directory || exit 1
	[[ -x ${format_binary##* } ]] || {
		echo "$name: format program \"${format_binary##* }\" not found"
		exit 1
	}
}

# Find the wt binary (required for abort/recovery testing).
wt_binary="../../wt"
[[ -x $wt_binary ]] || {
	echo "$name: wt program \"$wt_binary\" not found"
	exit 1
}

# We tested for the CONFIG file in the original directory, then in the WiredTiger source directory,
# the last place to check is in the WiredTiger build directory. Fail if we don't find it.
[[ $config_found -eq 0 ]] && {
    [[ -f "$config" ]] || {
	echo "$name: configuration file \"$config\" not found"
	exit 1
    }
}

verbose "$name configuration: $format_binary [-c $config]\
[-h $home] [-j $parallel_jobs] [-n $total_jobs] [-t $minutes] $format_args"

failure=0
success=0
running=0
status="format.sh-status"

# skip_known_errors
# return 0 - Error found and skip
# return 1 - skip_errors flag not set or no (known error) match found
skip_known_errors()
{
	# Return if "skip_errors" is not set or -E option is not passed
	[[ $skip_errors -ne 1 ]] && return 1

	log=$1

	# skip_error_list is a list of errors to skip. Each array entry can have multiple signatures
	# for finger-grained matching. For example:
	#
	#       err_1=("heap-buffer-overflow" "__split_parent")
	skip_error_list=( err_1[@] )

	# Loop through the skip list and search in the log file.
	err_count=${#skip_error_list[@]}
	for ((i=0; i<$err_count; i++))
	do
		# Tokenize the multi-signature error
		err_tokens[0]=${!skip_error_list[i]:0:1}
		err_tokens[1]=${!skip_error_list[i]:1:1}

		grep -q "${err_tokens[0]}" $log && grep -q "${err_tokens[1]}" $log

		[[ $? -eq 0 ]] && {
			echo "Skip error :  { ${err_tokens[0]} && ${err_tokens[1]} }"
			return 0
		}
	done
	return 1
}

# Categorize the failures
# $1 Log file
categorize_failure()
{
	log=$1

	# Add any important configs to be picked from the detailed failed configuration.
	configs=("backup=" "runs.source" "runs.type" "transaction.isolation" "transaction.rollback_to_stable"
			 "ops.prepare" "transaction.timestamps")
	count=${#configs[@]}

	search_string=""

	# now loop through the config array
	for ((i=0; i<$count; i++))
	do
		if [ $i == $(($count - 1)) ]
		then
			search_string+=${configs[i]}
		else
			search_string+="${configs[i]}|"
		fi
	done

	echo "############################################"
	echo "test/format run configuration highlights"
	echo "############################################"
	grep -E "$search_string" $log
	echo "############################################"
}

# Report a failure.
# $1 directory name
report_failure()
{
	# Note the directory may not yet exist, only the log file.
	dir=$1
	log="$dir.log"

	# DO NOT CURRENTLY SKIP ANY ERRORS.
	#skip_known_errors $log
	#skip_ret=$?

	failure=$(($failure + 1))

	# Forcibly quit if first-failure configured.
	[[ $first_failure -ne 0 ]] && force_quit=1

	echo "$name: job in $dir failed"
	sed 's/^/    /' < $log

	# Note the directory may not yet exist, only the log file. If the directory doesn't exist,
	# quit, we don't have any way to track that we've already reported this failure and it's
	# not worth the effort to try and figure one out, in all likelihood the configuration is
	# invalid.
	[[ -d "$dir" ]] || {
	    echo "$name: $dir does not exist, $name unable to continue"
	    force_quit=1
	    return
	}
	echo "$name: $dir/CONFIG:"
	sed 's/^/    /' < $dir/CONFIG

	categorize_failure $log

	echo "$name: failure status reported" > $dir/$status
}

# Resolve/cleanup completed jobs.
resolve()
{
	running=0
	list=$(ls $home | grep '^RUNDIR.[0-9]*.log')
	for i in $list; do
		# Note the directory may not yet exist, only the log file.
		dir="$home/${i%.*}"
		log="$home/$i"

		# Skip failures we've already reported.
		[[ -f "$dir/$status" ]] && continue

		# Leave any process waiting for a gdb attach running, but report it as a failure.
		grep -E 'waiting for debugger' $log > /dev/null && {
			report_failure $dir
			continue
		}

		# Get the process ID. There is a window where the PID might not yet be written, in
		# which case we ignore the log file. If the job is still running, ignore it unless
		# we're forcibly quitting. If it's not still running, wait for it and get an exit
		# status.
		pid=`awk '/process.*running/{print $3}' $log`
		[[ "$pid" =~ ^[1-9][0-9]*$ ]] || continue
		kill -s 0 $pid > /dev/null 2>&1 && {
			[[ $force_quit -eq 0 ]] && {
				running=$((running + 1))
				continue
			}

			# Kill the process group to catch any child processes.
			kill -KILL -- -$pid
			wait $pid

			# Remove jobs we killed, they count as neither success or failure.
			rm -rf $dir $log
			verbose "$name: job in $dir killed"
			continue
		}
		wait $pid
		eret=$?

		# Remove successful jobs.
		grep 'successful run completed' $log > /dev/null && {
			rm -rf $dir $log
			success=$(($success + 1))
			verbose "$name: job in $dir successfully completed"
			continue
		}

		# Check for Evergreen running out of disk space, and forcibly quit.
		grep -E -i 'no space left on device' $log > /dev/null && {
			rm -rf $dir $log
			force_quit=1
			echo "$name: job in $dir ran out of disk space"
			continue
		}

		# Test recovery on jobs configured for random abort. */
		grep 'aborting to test recovery' $log > /dev/null && {
			cp -pr $dir $dir.RECOVER

			(echo
			 echo "$name: running recovery after abort test"
			 echo "$name: original directory copied into $dir.RECOVER"
			 echo) >> $log

			# Verify the objects. In current format, it's a list of files named with a
			# leading F or tables named with a leading T. Historically, it was a file
			# or table named "wt".
			verify_failed=0
			for i in $(ls $dir | sed -e 's/.*\///'); do
			    case $i in
			    F*) uri="file:$i";;
			    T*) uri="table:${i%.wt}";;
			    wt) uri="file:wt";;
			    wt.wt) uri="table:wt";;
			    *) continue;;
			    esac

			    # Use the wt utility to recover & verify the object.
			    echo "verify: $wt_binary -m -R -h $dir verify $uri" >> $log
			    if  $($wt_binary -m -R -h $dir verify $uri >> $log 2>&1); then
				continue
			    fi

			    verify_failed=1
			    break
			done

			if [[ $verify_failed -eq 0 ]]; then
			    rm -rf $dir $dir.RECOVER $log
			    success=$(($success + 1))
			    verbose "$name: job in $dir successfully completed"
			else
			    echo "$name: job in $dir failed abort/recovery testing"
			    report_failure $dir
			fi
			continue
		}

		# Check for the library abort message, or an error from format.
		grep -E \
		    'aborting WiredTiger library|format alarm timed out|run FAILED' \
		    $log > /dev/null && {
			report_failure $dir
			continue
		}

		# There's some chance we just dropped core. We have the exit status of the process,
		# but there's no way to be sure. There are reasons the process' exit status looks
		# like a core dump was created (format deliberately causes a segfault in the case
		# of abort/recovery testing, and does work that can often segfault in the case of a
		# snapshot-isolation mismatch failure), but those cases have already been handled,
		# format is responsible for logging a failure before the core can happen. If the
		# process exited with a likely failure, call it a failure.
		signame=""
		case $eret in
		$((128 + 3)))
			signame="SIGQUIT";;
		$((128 + 4)))
			signame="SIGILL";;
		$((128 + 6)))
			signame="SIGABRT";;
		$((128 + 7)))
			signame="SIGBUS";;
		$((128 + 8)))
			signame="SIGFPE";;
		$((128 + 11)))
			signame="SIGSEGV";;
		$((128 + 24)))
			signame="SIGXCPU";;
		$((128 + 25)))
			signame="SIGXFSZ";;
		$((128 + 31)))
			signame="SIGSYS";;
		esac
		[[ -z $signame ]] || {
			(echo
			 echo "$name: job in $dir killed with signal $signame"
			 echo "$name: there may be a core dump associated with this failure"
			 echo) >> $log

			echo "$name: job in $dir killed with signal $signame"
			echo "$name: there may be a core dump associated with this failure"

			report_failure $dir
			continue
		}

		# If we don't understand why the job exited, report it as a failure and flag
		# a problem in this script.
		echo "$name: job in $dir exited with status $eret for an unknown reason"
		echo "$name: reporting job in $dir as a failure"
		report_failure $dir
	done
	return 0
}

# Start a single job.
count_jobs=0
format()
{
	count_jobs=$(($count_jobs + 1))
	dir="$home/RUNDIR.$count_jobs"
	log="$dir.log"

	args=""
	if [[ $smoke_test -ne 0 ]]; then
		args=${smoke_list[$smoke_next]}
		smoke_next=$(($smoke_next + 1))
	fi
	if [[ $directory_total -ne 0 ]]; then
		config="${directory_list[$directory_next]}"
		directory_next=$(($directory_next + 1))
	fi
	if [[ $abort_test -ne 0 ]]; then
		args+=" format.abort=1"
	fi
	if [[ $stress_split_test -ne 0 ]]; then
		for k in {1..8}; do
			args+=" stress_split_$k=$(($RANDOM%2))"
		done
	fi
	args+=" $format_args"
	echo "$name: starting job in $dir ($(date))"

	# If we're using UndoDB, append our default arguments.
	#
	# This script is typically left running until a failure is hit. To avoid filling up the
	# disk, we should avoid keeping recordings from successful runs.
	if [[ ! -z $live_record_binary ]]; then
		live_record_binary="$live_record_binary --save-on=error"
	fi

	cmd="$live_record_binary $format_binary -c "$config" -h "$dir" $args quiet=1"
	echo "$name: $cmd"

	# Disassociate the command from the shell script so we can exit and let the command
	# continue to run.
	# Run format in its own session so child processes are in their own process groups
	# and we can individually terminate (and clean up) running jobs and their children.
	nohup setsid $cmd > $log 2>&1 &

	# Check for setsid command failed execution, and forcibly quit (setsid exits 0 if the
	# command execution fails so we can't check the exit status). The RUNDIR directory is
	# not created in this failure type, check the log file explicitly.
	sleep 1
	grep -E -i 'setsid: failed to execute' $log > /dev/null && {
		failure=$(($failure + 1))
		force_quit=1
		echo "$name: job in $dir failed to execute"
	}
}

seconds=$((minutes * 60))
start_time="$(date -u +%s)"
while :; do
	# Check if our time has expired.
	[[ $seconds -ne 0 ]] && {
		now="$(date -u +%s)"
		elapsed=$(($now - $start_time))

		# If we've run out of time, terminate all running jobs.
		[[ $elapsed -ge $seconds ]] && {
			verbose "$name: run timed out at $(date)"
			force_quit=1
		}
	}

	# Check if we're only running the smoke-tests and we're done.
	[[ $smoke_test -ne 0 ]] && [[ $smoke_next -ge ${#smoke_list[@]} ]] && quit=1

	# Check if we're running CONFIGs from a directory and we're done.
	[[ $directory_total -ne 0 ]] && [[ $directory_next -ge $directory_total ]] && quit=1

	# Check if the total number of jobs has been reached.
	[[ $total_jobs -ne 0 ]] && [[ $count_jobs -ge $total_jobs ]] && quit=1

	# Check if less than 60 seconds left on any timer. The goal is to avoid killing jobs that
	# haven't yet configured signal handlers, because we rely on handler output to determine
	# their final status.
	[[ $seconds -ne 0 ]] && [[ $(($seconds - $elapsed)) -lt 60 ]] && quit=1

	# Start another job if we're not quitting for any reason and the maximum number of jobs
	# in parallel has not yet been reached.
	[[ $force_quit -eq 0 ]] && [[ $quit -eq 0 ]] && [[ $running -lt $parallel_jobs ]] && {
		running=$(($running + 1))
		format
	}

	# Clean up and update status.
	success_save=$success
	failure_save=$failure
	resolve
	[[ $success -ne $success_save ]] || [[ $failure -ne $failure_save ]] &&
	    echo "$name: $success successful jobs, $failure failed jobs"

	# Quit if we're done and there aren't any jobs left to wait for.
	[[ $quit -ne 0 ]] || [[ $force_quit -ne 0 ]] && [[ $running -eq 0 ]] && break

	# Wait for awhile, unless we're killing everything or there are jobs to start. Always wait
	# for a short period so we don't pound the system creating new jobs.
	[[ $force_quit -eq 0 ]] && [[ $running -ge $parallel_jobs ]] && sleep 8
	sleep 2
done

echo "$name: $success successful jobs, $failure failed jobs"

verbose "$name: run ending at $(date)"
[[ $failure -ne 0 ]] && exit 1
[[ $success -eq 0 ]] && exit 1
exit 0
