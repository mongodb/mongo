sleep 10s

mongo --host database1 --port 27018 --eval "config={\"_id\" : \"Shard1\",\"protocolVersion\" : 1,\"members\" : [{\"_id\" : 0,\"host\" : \"database1:27018\"}, {\"_id\" : 1,\"host\" : \"database2:27018\"}, {\"_id\" : 2,\"host\" : \"database3:27018\"} ],\"settings\" : {\"chainingAllowed\" : false,\"electionTimeoutMillis\" : 2000, \"heartbeatTimeoutSecs\" : 1, \"catchUpTimeoutMillis\": 0}}; rs.initiate(config)"

sleep 5s

mongo --host database4 --port 27018 --eval "config={\"_id\" : \"Shard2\",\"protocolVersion\" : 1,\"members\" : [{\"_id\" : 0,\"host\" : \"database4:27018\"}, {\"_id\" : 1,\"host\" : \"database5:27018\"}, {\"_id\" : 2,\"host\" : \"database6:27018\"} ],\"settings\" : {\"chainingAllowed\" : false,\"electionTimeoutMillis\" : 2000, \"heartbeatTimeoutSecs\" : 1, \"catchUpTimeoutMillis\": 0}}; rs.initiate(config)"

sleep 5s

mongo --host mongos --port 27017 --eval 'sh.addShard("Shard1/database1:27018,database2:27018,database3:27018")'

sleep 5s

mongo --host mongos --port 27017 --eval 'sh.addShard("Shard2/database4:27018,database5:27018,database6:27018")'

# this cryptic statement keeps the workload container running.
tail -f /dev/null
