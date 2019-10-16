/**
 * Tests upgrading a cluster from last stable to the latest version and downgrading it back to last
 * stable, verifying the behavior of chunk and zone operations throughout the process.
 */

// Checking UUID consistency uses cached connections, which are not valid across restarts or
// stepdowns.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
"use strict";

load("jstests/multiVersion/libs/config_chunks_tags_shared.js");
load("jstests/multiVersion/libs/multi_cluster.js");
load("jstests/multiVersion/libs/multi_rs.js");

// Runs commands on the config server that will use its RSM to target both shard primaries until
// they succeed.
function waitForConfigServerShardRSMRetarget(st) {
    assert.soonNoExcept(() => {
        assert.commandWorked(st.s.getDB("unrelated_db").unrelated_coll.insert({x: 1}));
        st.ensurePrimaryShard("unrelated_db", st.shard0.shardName);
        st.ensurePrimaryShard("unrelated_db", st.shard1.shardName);
        st.ensurePrimaryShard("unrelated_db", st.shard0.shardName);
        assert.commandWorked(st.s.getDB("unrelated_db").dropDatabase());
        return true;
    });
}

const dbName = "test";
const chunkNs = dbName + ".chunk_coll";
const zoneNs = dbName + ".zone_coll";

// Start a cluster with two shards at the last stable version and a sharding enabled db.
const st = new ShardingTest({
    shards: 2,
    other: {
        mongosOptions: {binVersion: "last-stable"},
        configOptions: {binVersion: "last-stable"},
        rsOptions: {binVersion: "last-stable"},
    },
    rs: {nodes: 3}  // Use 3 node replica sets to allow binary changes with no downtime.
});
checkFCV(st.configRS.getPrimary().getDB("admin"), lastStableFCV);
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

// Set up sharded collections for targeted chunk and zone operation testing.
setUpCollectionForChunksTesting(st, chunkNs);
setUpCollectionForZoneTesting(st, zoneNs);

// Set up another sharded collection on a different database to verify chunks and zones are updated
// for every sharded collection.
setUpExtraShardedCollections(st, "extra_db" /* dbName */);

// Set up collections with the same number of chunks and zones as the batch limit for the
// transactions used to modify chunks and zones documents and with more than the limit to verify the
// batching logic in both cases.
const txnBatchSize = 100;
setUpCollectionWithManyChunksAndZones(
    st, dbName + ".many_at_batch_size", txnBatchSize /* numChunks */, txnBatchSize /* numZones */);
setUpCollectionWithManyChunksAndZones(st,
                                      dbName + ".many_over_batch_size",
                                      txnBatchSize + 5 /* numChunks */,
                                      txnBatchSize + 5 /* numZones */);

//
// Upgrade back to the latest version.
//

verifyChunksAndTags(st, dbName, chunkNs, zoneNs, {expectNewFormat: false});

jsTestLog("Upgrading config servers.");
st.upgradeCluster("latest", {upgradeConfigs: true, upgradeMongos: false, upgradeShards: false});

verifyChunksAndTags(st, dbName, chunkNs, zoneNs, {expectNewFormat: false});

jsTestLog("Upgrading shard servers.");
st.upgradeCluster("latest", {upgradeConfigs: false, upgradeMongos: false, upgradeShards: true});

// Manually moving a chunk will use the config server's replica set monitor to target the primary of
// the source shard. After upgrading the shard servers above, this RSM may be stale, so run
// operations through the config server that will use the same RSM so it picks up the new primary.
waitForConfigServerShardRSMRetarget(st);

verifyChunksAndTags(st, dbName, chunkNs, zoneNs, {expectNewFormat: false});

jsTestLog("Upgrading mongos servers.");
st.upgradeCluster("latest", {upgradeConfigs: false, upgradeMongos: true, upgradeShards: false});
checkFCV(st.configRS.getPrimary().getDB("admin"), lastStableFCV);

verifyChunksAndTags(st, dbName, chunkNs, zoneNs, {expectNewFormat: false});

jsTestLog("Upgrade feature compatibility version to latest");
assert.commandWorked(st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(st.configRS.getPrimary().getDB("admin"), latestFCV);

verifyChunksAndTags(st, dbName, chunkNs, zoneNs, {expectNewFormat: true});

//
// Downgrade back to the last stable version.
//

jsTestLog("Downgrade feature compatibility version to last stable");
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
checkFCV(st.configRS.getPrimary().getDB("admin"), lastStableFCV);

verifyChunksAndTags(st, dbName, chunkNs, zoneNs, {expectNewFormat: false});

jsTestLog("Downgrading mongos servers.");
st.upgradeCluster("last-stable",
                  {upgradeConfigs: false, upgradeMongos: true, upgradeShards: false});

verifyChunksAndTags(st, dbName, chunkNs, zoneNs, {expectNewFormat: false});

jsTestLog("Downgrading shard servers.");
st.upgradeCluster("last-stable",
                  {upgradeConfigs: false, upgradeMongos: false, upgradeShards: true});

// Manually moving a chunk will use the config server's replica set monitor to target the primary of
// the source shard. After upgrading the shard servers above, this RSM may be stale, so run
// operations through the config server that will use the same RSM so it picks up the new primary.
waitForConfigServerShardRSMRetarget(st);

verifyChunksAndTags(st, dbName, chunkNs, zoneNs, {expectNewFormat: false});

jsTestLog("Downgrading config servers.");
st.upgradeCluster("last-stable",
                  {upgradeConfigs: true, upgradeMongos: false, upgradeShards: false});
checkFCV(st.configRS.getPrimary().getDB("admin"), lastStableFCV);

verifyChunksAndTags(st, dbName, chunkNs, zoneNs, {expectNewFormat: false});

st.stop();
})();
