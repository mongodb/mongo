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
