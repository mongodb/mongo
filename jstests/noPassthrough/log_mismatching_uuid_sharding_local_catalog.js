/*
 * Verifies a uuid mismatch between the sharding and the local catalog is correctly logged.
 */

TestData.skipCheckMetadataConsistency = true;

const st = new ShardingTest({shards: 2, config: 1});

const dbName = "test";
const collName = "coll";
const ns = dbName + "." + collName;
const primaryShard = st.shard1;
const primaryShardConn = st.rs1.getPrimary();

// Create a sharded collection. This will add an entry on both the sharding and the local
// catalog of the primary shard.
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShard.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: "hashed"}}));

// Drop and re-create the collection on the primary shard. This will generate a uuid mismatch.
primaryShardConn.getDB(dbName).getCollection(collName).drop();
assert.commandWorked(primaryShardConn.getDB(dbName).createCollection(collName));
// Re-create the shard key index for consistency. The shard key must always have an associated
// index. This issue would fail before the uuid mismach inconsistency is detected.
assert.commandWorked(
    primaryShardConn.getDB(dbName).getCollection(collName).createIndex({_id: "hashed"}));

// Run an insertion to force the shard to access the local catalog and detect the mismatch. The log
// should be eventually detected.
const logid = 9087200;
const timeoutMs = 5 * 60 * 1000;  // 5 min
const retryIntervalMS = 300;
assert.soon(
    () => {
        st.s.getDB(dbName).getCollection(collName).insert({x: 1});
        const logMsg = checkLog.getLogMessage(primaryShardConn, logid);
        if (logMsg) {
            return true;
        }
        return false;
    },
    'Could not find log entries containing the following message: ' + logid,
    timeoutMs,
    retryIntervalMS,
    {runHangAnalyzer: false});
st.stop();
