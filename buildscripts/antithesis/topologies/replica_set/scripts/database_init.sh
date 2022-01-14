mongod --bind_ip 0.0.0.0 --replSet RollbackFuzzerTest --logpath /var/log/mongodb/mongodb.log --setParameter enableTestCommands=1
# this cryptic statement keeps the container running.
tail -f /dev/null
