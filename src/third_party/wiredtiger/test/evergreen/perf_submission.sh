#!/bin/bash
# We use the requester expansion to determine whether the data is from a mainline evergreen run or not
if [ "${requester}" == "commit" ]; then
is_mainline=true
else
is_mainline=false
fi

# Parse the username out of the order_id. Patches append the username. The Signal Processing Service (SPS) endpoint does not need the other information.
parsed_order_id=$(echo "${revision_order_id}" | awk -F'_' '{print $NF}')

# Submit the performance data to the SPS endpoint
response=$(curl -s -w "\nHTTP_STATUS:%{http_code}" -X 'POST' \
"https://performance-monitoring-api.corp.mongodb.com/raw_perf_results/cedar_report?project=${project_id}&version=${version_id}&variant=${build_variant}&order=$parsed_order_id&task_name=${task_name}&task_id=${task_id}&execution=${execution}&mainline=$is_mainline" \
-H 'accept: application/json' \
-H 'Content-Type: application/json' \
-d @${perf_file_path})

http_status=$(echo "$response" | grep "HTTP_STATUS" | awk -F':' '{print $2}')
response_body=$(echo "$response" | sed '/HTTP_STATUS/d')

# We want to throw an error if the data was not successfully submitted
if [ "$http_status" -ne 200 ]; then
echo "Error: Received HTTP status $http_status"
echo "Response Body: $response_body"
exit 1
fi

echo "Response Body: $response_body"
echo "HTTP Status: $http_status"
