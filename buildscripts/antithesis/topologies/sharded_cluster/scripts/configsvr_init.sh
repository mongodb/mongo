mongod --bind_ip 0.0.0.0 --configsvr --replSet ConfigServerReplSet --logpath /var/log/mongodb/mongodb.log --setParameter enableTestCommands=1 --setParameter reshardingMinimumOperationDurationMillis=30000 --setParameter fassertOnLockTimeoutForStepUpDown=0 --wiredTigerCacheSizeGB 1 --oplogSize 256

# this cryptic statement keeps the container running.
tail -f /dev/null
