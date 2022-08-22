"""Script to configure a sharded cluster in Antithesis from the mongos container."""
import json
import subprocess
from time import sleep
from utils import mongo_process_running, retry_until_success

CONFIGSVR_CONFIG = {
    "_id": "ConfigServerReplSet",
    "configsvr": True,
    "protocolVersion": 1,
    "members": [
        {"_id": 0, "host": "configsvr1:27019"},
        {"_id": 1, "host": "configsvr2:27019"},
        {"_id": 2, "host": "configsvr3:27019"},
    ],
    "settings": {
        "chainingAllowed": False,
        "electionTimeoutMillis": 2000,
        "heartbeatTimeoutSecs": 1,
        "catchUpTimeoutMillis": 0,
    },
}

SHARD1_CONFIG = {
    "_id": "Shard1",
    "protocolVersion": 1,
    "members": [
        {"_id": 0, "host": "database1:27018"},
        {"_id": 1, "host": "database2:27018"},
        {"_id": 2, "host": "database3:27018"},
    ],
    "settings": {
        "chainingAllowed": False,
        "electionTimeoutMillis": 2000,
        "heartbeatTimeoutSecs": 1,
        "catchUpTimeoutMillis": 0,
    },
}

SHARD2_CONFIG = {
    "_id": "Shard2",
    "protocolVersion": 1,
    "members": [
        {"_id": 0, "host": "database4:27018"},
        {"_id": 1, "host": "database5:27018"},
        {"_id": 2, "host": "database6:27018"},
    ],
    "settings": {
        "chainingAllowed": False,
        "electionTimeoutMillis": 2000,
        "heartbeatTimeoutSecs": 1,
        "catchUpTimeoutMillis": 0,
    },
}

# Create ConfigServerReplSet once all nodes are up
retry_until_success(mongo_process_running, {"host": "configsvr1", "port": 27019})
retry_until_success(mongo_process_running, {"host": "configsvr2", "port": 27019})
retry_until_success(mongo_process_running, {"host": "configsvr3", "port": 27019})
retry_until_success(
    subprocess.run, {
        "args": [
            "mongo",
            "--host",
            "configsvr1",
            "--port",
            "27019",
            "--eval",
            f"rs.initiate({json.dumps(CONFIGSVR_CONFIG)})",
        ],
        "check": True,
    })

# Create Shard1 once all nodes are up
retry_until_success(mongo_process_running, {"host": "database1", "port": 27018})
retry_until_success(mongo_process_running, {"host": "database2", "port": 27018})
retry_until_success(mongo_process_running, {"host": "database3", "port": 27018})
retry_until_success(
    subprocess.run, {
        "args": [
            "mongo",
            "--host",
            "database1",
            "--port",
            "27018",
            "--eval",
            f"rs.initiate({json.dumps(SHARD1_CONFIG)})",
        ],
        "check": True,
    })

# Create Shard2 once all nodes are up
retry_until_success(mongo_process_running, {"host": "database4", "port": 27018})
retry_until_success(mongo_process_running, {"host": "database5", "port": 27018})
retry_until_success(mongo_process_running, {"host": "database6", "port": 27018})
retry_until_success(
    subprocess.run, {
        "args": [
            "mongo",
            "--host",
            "database4",
            "--port",
            "27018",
            "--eval",
            f"rs.initiate({json.dumps(SHARD2_CONFIG)})",
        ],
        "check": True,
    })

# Start mongos
retry_until_success(
    subprocess.run, {
        "args": [
            "mongos",
            "--bind_ip",
            "0.0.0.0",
            "--configdb",
            "ConfigServerReplSet/configsvr1:27019,configsvr2:27019,configsvr3:27019",
            "--logpath",
            "/var/log/mongodb/mongodb.log",
            "--setParameter",
            "enableTestCommands=1",
            "--setParameter",
            "fassertOnLockTimeoutForStepUpDown=0",
            "--fork",
        ],
        "check": True,
    })

# Add shards to cluster
retry_until_success(
    subprocess.run, {
        "args": [
            "mongo",
            "--host",
            "mongos",
            "--port",
            "27017",
            "--eval",
            'sh.addShard("Shard1/database1:27018,database2:27018,database3:27018")',
        ],
        "check": True,
    })
retry_until_success(
    subprocess.run, {
        "args": [
            "mongo",
            "--host",
            "mongos",
            "--port",
            "27017",
            "--eval",
            'sh.addShard("Shard2/database4:27018,database5:27018,database6:27018")',
        ],
        "check": True,
    })

while True:
    sleep(10)
