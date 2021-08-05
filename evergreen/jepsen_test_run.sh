DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src/jepsen-mongodb

set -o verbose

# Set the TMPDIR environment variable to be a directory in the task's working
# directory so that temporary files created by processes spawned by jepsen get
# cleaned up after the task completes. This also ensures the spawned processes
# aren't impacted by limited space in the mount point for the /tmp directory.
# We also need to set the _JAVA_OPTIONS environment variable so that lein will
# recognize this as the default temp directory.
export TMPDIR="${workdir}/tmp"
mkdir -p $TMPDIR
export _JAVA_OPTIONS=-Djava.io.tmpdir=$TMPDIR

start_time=$(date +%s)
lein run test --test ${jepsen_test_name} \
  --mongodb-dir ../ \
  --working-dir ${workdir}/src/jepsen-workdir \
  --clock-skew faketime \
  --libfaketime-path ${workdir}/src/libfaketime/build/libfaketime.so.1 \
  --mongod-conf mongod_verbose.conf \
  --virtualization none \
  --nodes-file ../nodes.txt \
  ${jepsen_key_time_limit} \
  ${jepsen_protocol_version} \
  ${jepsen_read_concern} \
  ${jepsen_read_with_find_and_modify} \
  ${jepsen_storage_engine} \
  ${jepsen_time_limit} \
  ${jepsen_write_concern} \
  2>&1 \
  | tee jepsen_${task_name}_${execution}.log
end_time=$(date +%s)
elapsed_secs=$((end_time - start_time))
# Since we cannot use PIPESTATUS to get the exit code from the "lein run ..." pipe in dash shell,
# we will check the output for success, failure or setup error. Note that 'grep' returns with exit code
# 0 if it finds a match, and exit code 1 if no match is found.
grep -q "Everything looks good" jepsen_${task_name}_${execution}.log
grep_exit_code=$?
if [ $grep_exit_code -eq 0 ]; then
  status='"pass"'
  failures=0
  final_exit_code=0
else
  grep -q "Analysis invalid" jepsen_${task_name}_${execution}.log
  grep_exit_code=$?
  if [ $grep_exit_code -eq 0 ]; then
    status='"fail"'
    failures=1
    final_exit_code=1
  else
    # If the failure is due to setup, then this is considered a system failure.
    echo $grep_exit_code > jepsen_system_failure_${task_name}_${execution}
    exit 0
  fi
fi
# Create report.json
echo "{\"failures\": $failures, \"results\": [{\"status\": $status, \"exit_code\": $final_exit_code, \"test_file\": \"${task_name}\", \"start\": $start_time, \"end\": $end_time, \"elapsed\": $elapsed_secs}]}" > ../report.json
exit $final_exit_code
