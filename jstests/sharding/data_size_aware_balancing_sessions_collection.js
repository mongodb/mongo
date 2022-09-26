/*
 * Tests that the balancer splits the sessions collection and uniformly distributes the chunks
 * across shards in the cluster.
 * @tags: [
 * featureFlagBalanceAccordingToDataSize,
 * requires_fcv_61,
 * resource_intensive,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");
load("jstests/sharding/libs/find_chunks_util.js");
load('jstests/sharding/libs/remove_shard_util.js');

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

/*
 * Returns the number of chunks for the sessions collection.
 */
function getNumTotalChunks() {
    return findChunksUtil.countChunksForNs(configDB, kSessionsNs);
}

/*
 * Returns the number of chunks for the sessions collection that are the given shard.
 */
function getNumChunksOnShard(shardName) {
    return findChunksUtil.countChunksForNs(configDB, kSessionsNs, {shard: shardName});
}

/*
 * Returns the number of docs in the sessions collection on the given host.
 */
function getNumSessionDocs(conn) {
    return conn.getCollection(kSessionsNs).find().itcount();
}

/*
 * Starts a replica-set shard, adds the shard to the cluster, and increments numShards.
 * Returns the ReplSetTest object for the shard.
 */
function addShardsToCluster(shardsToAdd) {
    let addedReplicaSets = [];
    for (let i = 0; i < shardsToAdd; ++i) {
        const shardName = clusterName + "-rs" + numShards;
        const replTest = new ReplSetTest({name: shardName, nodes: 1});
        replTest.startSet({shardsvr: ""});
        replTest.initiate();

        assert.commandWorked(st.s.adminCommand({addShard: replTest.getURL(), name: shardName}));
        numShards++;
        addedReplicaSets.push(replTest);
    }
    return addedReplicaSets;
}

/*
 * Removes the given shard from the cluster, waits util the state is completed, and
 * decrements numShards.
 */
function removeShardFromCluster(shardName) {
    removeShard(st, shardName, kBalancerTimeoutMS);
    numShards--;
}

/*
 * Returns the estimated size (in bytes) of the sessions collection chunks hosted by the shard.
 */
function getSessionsCollSizeInShard(shardStats) {
    const orphansSize =
        shardStats['storageStats']['numOrphanDocs'] * shardStats['storageStats']['avgObjSize'];
    return shardStats['storageStats']['size'] - orphansSize;
}

function printSessionsCollectionDistribution(shards) {
    const numDocsOnShards = shards.map(shard => getNumSessionDocs(shard));
    const collStatsPipeline = [
        {'$collStats': {'storageStats': {}}},
        {
            '$project': {
                'shard': true,
                'storageStats':
                    {'count': true, 'size': true, 'avgObjSize': true, 'numOrphanDocs': true}
            }
        },
        {'$sort': {'shard': 1}}
    ];
    const collectionStorageStats =
        st.s.getCollection(kSessionsNs).aggregate(collStatsPipeline).toArray();
    const collSizeDistribution =
        collectionStorageStats.map(shardStats => getSessionsCollSizeInShard(shardStats));
    const numChunksOnShard = shards.map(shard => getNumChunksOnShard(shard.shardName));
    const kMaxChunkSizeBytes = st.config.collections.findOne({_id: kSessionsNs}).maxChunkSizeBytes;

    jsTest.log(`Sessions distribution across shards ${tojson(shards)}: #docs = ${
        tojson(numDocsOnShards)}, #chunks = ${tojson(numChunksOnShard)}, size = ${
        tojson(collSizeDistribution)}, #maxChunkSize: ${tojson(kMaxChunkSizeBytes)}`);
}

function waitUntilBalancedAndVerify(shards) {
    const coll = st.s.getCollection(kSessionsNs);
    st.awaitBalance(
        kSessionsCollName, kConfigDbName, 9 * 60000 /* 9min timeout */, 1000 /* 1s interval */);
    printSessionsCollectionDistribution(shards);
    st.verifyCollectionIsBalanced(coll);
}

const kMinNumChunks = 100;
const kExpectedNumChunks = 128;  // the balancer rounds kMinNumChunks to the next power of 2.
const kNumSessions = 2000;
const kBalancerTimeoutMS = 5 * 60 * 1000;

let numShards = 2;
const clusterName = jsTest.name();
const st = new ShardingTest({
    name: clusterName,
    shards: numShards,
    other: {configOptions: {setParameter: {minNumChunksForSessionsCollection: kMinNumChunks}}}
});

const kConfigDbName = "config";
const kSessionsCollName = "system.sessions";
const kSessionsNs = `${kConfigDbName}.${kSessionsCollName}`;
const configDB = st.s.getDB(kConfigDbName);

// There is only one chunk initially.
assert.eq(1, getNumTotalChunks());

st.startBalancer();

jsTest.log(
    `Verify that the balancer generates the expected initial set of chunks for ${kSessionsNs}`);

assert.soon(() => getNumTotalChunks() == kExpectedNumChunks,
            "balancer did not split the initial chunk for the sessions collection");

jsTest.log(`Verify that no chunks are moved from the primary shard of ${
    kSessionsNs} if the are no open sessions`);
{
    st.awaitBalance(kSessionsCollName, kConfigDbName);
    const numChunksInShard0 = getNumChunksOnShard(st.shard0.shardName);
    const numChunksInShard1 = getNumChunksOnShard(st.shard1.shardName);
    assert(numChunksInShard0 === kExpectedNumChunks && numChunksInShard1 === 0 ||
           numChunksInShard1 === kExpectedNumChunks && numChunksInShard0 === 0);
}

jsTest.log(`Creating ${kNumSessions} sessions`);
for (let i = 0; i < kNumSessions; i++) {
    assert.commandWorked(st.s.adminCommand({startSession: 1}));
}
assert.commandWorked(st.s.adminCommand({refreshLogicalSessionCacheNow: 1}));
assert.lte(kNumSessions, getNumSessionDocs(st.s));
let shards = [st.shard0, st.shard1];
jsTest.log(`Verify that the chunks of ${kSessionsNs} get distributed across the original cluster`);
waitUntilBalancedAndVerify(shards);

jsTest.log(
    "Verify that the balancer redistributes chunks when more shards are added to the cluster");
const addedReplicaSets = addShardsToCluster(3);
shards = shards.concat(addedReplicaSets.map(rs => {
    const primaryNode = rs.getPrimary();
    primaryNode.shardName = rs.name;
    return primaryNode;
}));
waitUntilBalancedAndVerify(shards);

jsTest.log(
    "Verify that the balancer redistributes chunks when shards are removed from the cluster");
removeShardFromCluster(shards[2].shardName);
shards.splice(2, 1);
waitUntilBalancedAndVerify(shards);

st.stopBalancer();

st.stop();
addedReplicaSets.forEach(rs => {
    rs.stopSet();
});
}());
