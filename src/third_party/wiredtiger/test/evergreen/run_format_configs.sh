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

                # Grep for the exact process id in the temp file.
                config_name=`grep -E -w "${process}" $tmp_file | awk -F ":" '{print $2}' \
                    | rev | awk -F "/" '{print $1}' | rev`
                dir="WT_TEST_$config_name"
                log="WT_TEST_$config_name.log"

                # Test recovery of jobs configured for random abort.
                if [ -f "$log" ] && grep -q 'aborting to test recovery' "$log" 2>/dev/null; then
                    rec_dir="$dir.RECOVER"
                    cp -pr $dir $rec_dir

                    (echo
                    echo "Running recovery after abort test"
                    echo "Original directory copied into $rec_dir"
                    echo) >> $log

                    ./t -Rqv -h $dir >> $log 2>&1 &
                    rec_pid="$!"
                    wait $rec_pid
                    exit_status=$?

                    echo >> $log
                    echo "Exit status of recovery process ${rec_pid} is ${exit_status}" >> $log
                    if [ $exit_status -eq 0 ]; then
                        rm -rf $rec_dir
                    fi
                else
                    echo >> $log
                    echo "Exit status of pid ${process} is ${exit_status}" >> $log
                fi

                # Print the log if it exists.
                echo
                echo "Log for config ${config_name}:"
                [ -f $log ] && sed "s/^/${config_name}: /" $log
                echo

                # We are done.
                let "running--"
                if [ $exit_status -ne "0" ]; then
                    let "failure++"
                    [ -f $dir/CONFIG ] && cat $dir/CONFIG
                else
                    let "success++"
                    # Remove database files of successful jobs.
                    [ -f $log ] && rm -f $log
                    [ -d $dir ] && rm -rf $dir
                fi

                echo "Exit status for config ${config_name} is ${exit_status}"
                # Continue checking other running process status before exiting the for loop.
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
    echo -e "\nTesting CONFIG $config ..."
    basename_config=$(basename $config)
    log="WT_TEST_$basename_config.log"
    ./t -1 -c $config -h WT_TEST_$basename_config > $log 2>&1 &
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
