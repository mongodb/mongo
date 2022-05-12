sleep 5s
mongo --host configsvr1 --port 27019 --eval "config={\"_id\" : \"ConfigServerReplSet\",\"configsvr\" : true,\"protocolVersion\" : 1,\"members\" : [{\"_id\" : 0,\"host\" : \"configsvr1:27019\"}, {\"_id\" : 1,\"host\" : \"configsvr2:27019\"}, {\"_id\" : 2,\"host\" : \"configsvr3:27019\"} ],\"settings\" : {\"chainingAllowed\" : false,\"electionTimeoutMillis\" : 2000, \"heartbeatTimeoutSecs\" : 1, \"catchUpTimeoutMillis\": 0}}; rs.initiate(config)"

mongos --bind_ip 0.0.0.0 --configdb ConfigServerReplSet/configsvr1:27019,configsvr2:27019,configsvr3:27019 --logpath /var/log/mongodb/mongodb.log --setParameter enableTestCommands=1 --setParameter fassertOnLockTimeoutForStepUpDown=0

# this cryptic statement keeps the container running.
tail -f /dev/null
