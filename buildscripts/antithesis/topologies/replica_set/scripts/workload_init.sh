sleep 5s
mongo --host database1 --port 27017 --eval "config={\"_id\" : \"RollbackFuzzerTest\",\"protocolVersion\" : 1,\"members\" : [{\"_id\" : 0,\"host\" : \"database1:27017\"}, {\"_id\" : 1,\"host\" : \"database2:27017\"}, {\"_id\" : 2,\"host\" : \"database3:27017\"} ],\"settings\" : {\"chainingAllowed\" : false,\"electionTimeoutMillis\" : 500, \"heartbeatTimeoutSecs\" : 1, \"catchUpTimeoutMillis\": 700}}; rs.initiate(config)"

# this cryptic statement keeps the workload container running.
tail -f /dev/null
