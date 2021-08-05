DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

report_file="report.json"
# Check if the report file exists and has failures.
if [ -f $report_file ] && grep -Eq "\"failures\": [1-9]" $report_file; then
  # Calling the BF Suggestion server endpoint to start feature extraction.
  payload="{\"task_id\": \"${task_id}\", \"execution\": ${execution}}"
  echo "Sending task info to the BF suggestion service"
  # The --user option is passed through stdin to avoid showing in process list.
  user_option="--user ${bfsuggestion_user}:${bfsuggestion_password}"
  curl --header "Content-Type: application/json" \
    --data "$payload" \
    --max-time 10 \
    --silent \
    --show-error \
    --config - \
    https://bfsuggestion.corp.mongodb.com/tasks <<< $user_option
  echo "Request to BF Suggestion service status: $?"
fi
