// Tests changing the zones on a shard at runtime results in a correct distribution of chunks across
// the cluster
(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");

const st = new ShardingTest({shards: 3, mongos: 1, other: {chunkSize: 1, enableAutoSplit: false}});
const dbName = 'test';
const collName = 'foo';
const ns = dbName + '.' + collName;

assert.commandWorked(
    st.s0.adminCommand({enablesharding: dbName, primaryShard: st.shard1.shardName}));

var testDB = st.s0.getDB(dbName);
var configDB = st.s0.getDB('config');

assert.commandWorked(st.s0.adminCommand({shardCollection: ns, key: {_id: 1}}));

const bigString = 'X'.repeat(1024 * 1024);  // 1MB
var bulk = testDB.foo.initializeUnorderedBulkOp();
for (var i = 0; i < 9; i++) {
    bulk.insert({_id: i, x: bigString});
}
assert.commandWorked(bulk.execute());

// Produce 9 chunks with min  value at the documents just inserted
for (var i = 0; i < 8; i++) {
    assert.commandWorked(st.s0.adminCommand({split: ns, middle: {_id: i}}));
}

/**
 * Waits for the balancer state described by the checking logic function (checkFunc) to be
 * reached and ensures that it does not change from that state at the next balancer round.
 */
function assertBalanceCompleteAndStable(checkFunc, stepName) {
    st.printShardingStatus(true);

    assert.soon(checkFunc, 'Balance at step ' + stepName + ' did not happen', 3 * 60 * 1000, 2000);

    st.awaitBalancerRound();
    st.printShardingStatus(true);
    assert(checkFunc());

    jsTestLog('Completed step ' + stepName);
}

/**
 * Checker function to be used with assertBalanceCompleteAndStable, which ensures that the
 * cluster is evenly balanced.
 */
function checkClusterEvenlyBalanced() {
    assert.commandWorked(st.s.getDB('admin').runCommand({balancerStatus: 1}));
    return true;
}

st.startBalancer();

// Initial balance
assertBalanceCompleteAndStable(checkClusterEvenlyBalanced, 'initial');

// Spread chunks correctly across zones
st.addShardTag(st.shard0.shardName, 'a');
st.addShardTag(st.shard1.shardName, 'a');
st.addTagRange(ns, {_id: -100}, {_id: 100}, 'a');

st.addShardTag(st.shard2.shardName, 'b');
st.addTagRange(ns, {_id: MinKey}, {_id: -100}, 'b');
st.addTagRange(ns, {_id: 100}, {_id: MaxKey}, 'b');

assertBalanceCompleteAndStable(function() {
    var chunksOnShard2 = findChunksUtil.findChunksByNs(configDB, ns, {shard: st.shard2.shardName})
                             .sort({min: 1})
                             .toArray();

    jsTestLog('Chunks on shard2: ' + tojson(chunksOnShard2));

    if (chunksOnShard2.length != 2) {
        return false;
    }

    return chunksOnShard2[0].min._id == MinKey && chunksOnShard2[0].max._id == -100 &&
        chunksOnShard2[1].min._id == 100 && chunksOnShard2[1].max._id == MaxKey;
}, 'chunks to zones a and b');

// Tag the entire collection to shard0 and wait for everything to move to that shard
st.removeTagRange(ns, {_id: -100}, {_id: 100}, 'a');
st.removeTagRange(ns, {_id: MinKey}, {_id: -100}, 'b');
st.removeTagRange(ns, {_id: 100}, {_id: MaxKey}, 'b');

st.removeShardTag(st.shard1.shardName, 'a');
st.removeShardTag(st.shard2.shardName, 'b');
st.addTagRange(ns, {_id: MinKey}, {_id: MaxKey}, 'a');

assertBalanceCompleteAndStable(function() {
    var counts = st.chunkCounts(collName);
    printjson(counts);
    return counts[st.shard0.shardName] == 11 && counts[st.shard1.shardName] == 0 &&
        counts[st.shard2.shardName] == 0;
}, 'all chunks to zone a');

// Remove all zones and ensure collection is correctly redistributed
st.removeShardTag(st.shard0.shardName, 'a');
st.removeTagRange(ns, {_id: MinKey}, {_id: MaxKey}, 'a');

assertBalanceCompleteAndStable(checkClusterEvenlyBalanced, 'final');

st.stop();
})();
