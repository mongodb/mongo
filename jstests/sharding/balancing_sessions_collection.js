/*
 * Tests that the balancer splits the sessions collection and uniformly distributes the chunks
 * across shards in the cluster.
 * @tags: [resource_intensive]
 */
(function() {
"use strict";

/*
 * Returns the number of chunks for the sessions collection.
 */
function getNumTotalChunks() {
    return configDB.chunks.count({ns: kSessionsNs});
}

/*
 * Returns the number of chunks for the sessions collection that are the given shard.
 */
function getNumChunksOnShard(shardName) {
    return configDB.chunks.count({ns: kSessionsNs, shard: shardName});
}

/*
 * Returns the number of docs in the sessions collection on the given host.
 */
function getNumDocs(conn) {
    return conn.getCollection(kSessionsNs).count();
}

/*
 * Starts a replica-set shard, adds the shard to the cluster, and increments numShards.
 * Returns the ReplSetTest object for the shard.
 */
function addShardToCluster() {
    const shardName = clusterName + "-rs" + numShards;

    const replTest = new ReplSetTest({name: shardName, nodes: 1});
    replTest.startSet({shardsvr: ""});
    replTest.initiate();

    assert.commandWorked(st.s.adminCommand({addShard: replTest.getURL(), name: shardName}));
    numShards++;
    return replTest;
}

/*
 * Removes the given shard from the cluster, waits util the state is completed, and
 * decrements numShards.
 */
function removeShardFromCluster(shardName) {
    assert.commandWorked(st.s.adminCommand({removeShard: shardName}));
    assert.soon(function() {
        const res = st.s.adminCommand({removeShard: shardName});
        assert.commandWorked(res);
        return ("completed" == res.state);
    }, "failed to remove shard " + shardName, kBalancerTimeoutMS);
    numShards--;
}

/*
 * Returns true if the chunks for the sessions collection are evenly distributed across the
 * given shards. That is, the number of chunks on the most loaded shard and on the least
 * loaded shard differs by no more than 1.
 */
function isBalanced(shardNames) {
    const expectedMinNumChunksPerShard = Math.floor(kExpectedNumChunks / shardNames.length);

    let minNumChunks = Number.MAX_VALUE;
    let maxNumChunks = 0;
    for (const shardName of shardNames) {
        const numChunks = getNumChunksOnShard(shardName);
        minNumChunks = Math.min(numChunks, minNumChunks);
        maxNumChunks = Math.max(numChunks, maxNumChunks);
    }

    return (maxNumChunks - minNumChunks <= 1) && (minNumChunks == expectedMinNumChunksPerShard);
}

/*
 * Returns the standard deviation for given numbers.
 */
function computeStdDev(nums) {
    const mean = nums.reduce((a, b) => a + b) / nums.length;
    return Math.sqrt(nums.map(x => Math.pow(x - mean, 2)).reduce((a, b) => a + b) / nums.length);
}

const kMinNumChunks = 100;
const kExpectedNumChunks = 128;  // the balancer rounds kMinNumChunks to the next power of 2.
const kNumSessions = 10000;
const kBalancerTimeoutMS = 5 * 60 * 1000;

let numShards = 2;
const clusterName = jsTest.name();
const st = new ShardingTest({
    name: clusterName,
    shards: numShards,
    other: {configOptions: {setParameter: {minNumChunksForSessionsCollection: kMinNumChunks}}}
});
const kSessionsNs = "config.system.sessions";
const configDB = st.s.getDB("config");

// There is only one chunk initially.
assert.eq(1, getNumTotalChunks());

st.startBalancer();

jsTest.log(
    "Verify that the balancer splits the initial chunks and distributes chunks evenly across existing shards");

assert.soon(() => getNumTotalChunks() == kExpectedNumChunks,
            "balancer did not split the initial chunk for the sessions collection");
assert.soon(() => isBalanced([st.shard0.shardName, st.shard1.shardName]),
            "balancer did not distribute chunks evenly across existing shards",
            kBalancerTimeoutMS);

jsTest.log(
    "Verify that the balancer redistributes chunks when more shards are added to the cluster");
const shard2 = addShardToCluster();
const shard3 = addShardToCluster();
const shard4 = addShardToCluster();

assert.soon(() => isBalanced(
                [st.shard0.shardName, st.shard1.shardName, shard2.name, shard3.name, shard4.name]),
            "balancer did not redistribute chunks evenly after more shards were added",
            kBalancerTimeoutMS);

jsTest.log("Verify that the session docs are distributed almost evenly across shards");
// Start sessions and trigger a refresh to flush the sessions to the sessions collection.
for (let i = 0; i < kNumSessions; i++) {
    assert.commandWorked(st.s.adminCommand({startSession: 1}));
}
assert.commandWorked(st.s.adminCommand({refreshLogicalSessionCacheNow: 1}));
assert.lte(kNumSessions, getNumDocs(st.s));

const shards =
    [st.shard0, st.shard1, shard2.getPrimary(), shard3.getPrimary(), shard4.getPrimary()];
const numDocsOnShards = shards.map(shard => getNumDocs(shard));
assert.lt(computeStdDev(numDocsOnShards), 0.1 * kNumSessions / shards.length);

jsTest.log(
    "Verify that the balancer redistributes chunks when shards are removed from the cluster");
removeShardFromCluster(shard2.name);

assert.soon(() => isBalanced([st.shard0.shardName, st.shard1.shardName, shard3.name, shard4.name]),
            "balancer did not redistribute chunks evenly after one of the shards was removed",
            kBalancerTimeoutMS);
assert.eq(0, getNumChunksOnShard(shard2.name));

st.stopBalancer();

st.stop();
shard2.stopSet();
shard3.stopSet();
shard4.stopSet();
}());
