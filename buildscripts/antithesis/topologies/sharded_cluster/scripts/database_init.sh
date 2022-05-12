mongod --bind_ip 0.0.0.0 --shardsvr --replSet "$1" --logpath /var/log/mongodb/mongodb.log --oplogSize 256 --setParameter enableTestCommands=1 --setParameter enableElectionHandoff=0 --setParameter roleGraphInvalidationIsFatal=1 --setParameter receiveChunkWaitForRangeDeleterTimeoutMS=90000 --setParameter fassertOnLockTimeoutForStepUpDown=0 --wiredTigerCacheSizeGB 1

# this cryptic statement keeps the container running.
tail -f /dev/null
