/*
 * Test that the balancer is redistributing data based on the actual amount of data
 * for a collection on each node, converging when the size difference becomes small.
 *
 * @tags: [
 *     featureFlagBalanceAccordingToDataSize,
 *     requires_fcv_61,
 * ]
 */

(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");

function getCollSizeMB(ns, node) {
    let res;
    let collections = [{ns: ns}];
    assert.soon(() => {
        res = assert.commandWorkedOrFailedWithCode(
            node.adminCommand({_shardsvrGetStatsForBalancing: 1, collections: collections}),
            [ErrorCodes.NotYetInitialized]);
        return res.ok;
    });

    return res['stats'][0]['collSize'];
}

const maxChunkSizeMB = 1;
const st = new ShardingTest(
    {shards: 2, mongos: 1, other: {chunkSize: maxChunkSizeMB, enableBalancer: false}});
const dbName = 'test';
const coll = st.getDB(dbName).getCollection('foo');
const ns = coll.getFullName();
const mongos = st.s;
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;

// Shard collection with one chunk on shard0 [MinKey, 0) and one chunk on shard1 [0, MinKey)
assert.commandWorked(mongos.adminCommand({enablesharding: dbName, primaryShard: shard0}));
assert.commandWorked(mongos.adminCommand({shardcollection: ns, key: {_id: 1}}));
assert.commandWorked(mongos.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {_id: 0}, to: shard1}));

const bigString = 'X'.repeat(1024 * 1024);  // 1MB

// Insert 10MB of documents in range [MinKey, 0) on shard0
var bulk = coll.initializeUnorderedBulkOp();
for (var i = -1; i > -11; i--) {
    bulk.insert({_id: i, s: bigString});
}
assert.commandWorked(bulk.execute());

// Insert 3MB of documents in range [0, MaxKey) on shard1
bulk = coll.initializeUnorderedBulkOp();
for (var i = 0; i < 3; i++) {
    bulk.insert({_id: i, s: bigString});
}
assert.commandWorked(bulk.execute());

// Create 3 more chunks on shard0
assert.commandWorked(mongos.adminCommand({split: ns, middle: {_id: 1}}));
assert.commandWorked(mongos.adminCommand({split: ns, middle: {_id: 2}}));
assert.commandWorked(mongos.adminCommand({split: ns, middle: {_id: 3}}));

// At this point, the distribution of chunks for the testing collection is the following:
//  - On shard0 (10MB):
//     { "_id" : { "$minKey" : 1 } } -->> { "_id" : 0 }
//  - On shard1 (3MB):
//     { "_id" : 0 } -->> { "_id" : 1 }
//     { "_id" : 1 } -->> { "_id" : 2 }
//     { "_id" : 2 } -->> { "_id" : 3 }
//     { "_id" : 3 } -->> { "_id" : { "$maxKey" : 1 } }
jsTestLog("Printing sharding status before starting balancer");
st.printShardingStatus();
st.startBalancer();

assert.soon(function() {
    return assert.commandWorked(st.s0.adminCommand({balancerCollectionStatus: ns}))
        .balancerCompliant;
}, 'Timed out waiting for the collection to be balanced', 60000 /* timeout */, 1000 /* interval */);

// Check that the collection size diff between shards is small (2 * maxChunkSize)
const collSizeOnShard0BeforeNoopRounds = getCollSizeMB(ns, st.shard0.rs.getPrimary());
const collSizeOnShard1BeforeNoopRounds = getCollSizeMB(ns, st.shard1.rs.getPrimary());
const chunksBeforeNoopRound = findChunksUtil.findChunksByNs(st.config, ns).toArray();
var errMsg = '[Before noop round] Data on shard0 = ' + collSizeOnShard0BeforeNoopRounds +
    ' and data on shard 1 = ' + collSizeOnShard1BeforeNoopRounds +
    ' - chunks before noop round = ' + JSON.stringify(chunksBeforeNoopRound);
assert.lte(collSizeOnShard0BeforeNoopRounds - collSizeOnShard1BeforeNoopRounds,
           2 * maxChunkSizeMB,
           errMsg);

// Wait for some more rounds and then check the balancer is not wrongly moving around data
st.forEachConfigServer((conn) => {
    conn.adminCommand({
        configureFailPoint: 'overrideBalanceRoundInterval',
        mode: 'alwaysOn',
        data: {intervalMs: 100}
    });
});

st.awaitBalancerRound();
st.awaitBalancerRound();
st.awaitBalancerRound();

st.stopBalancer();
jsTestLog("Printing sharding status after stopping balancer");
st.printShardingStatus();

const collSizeOnShard0AfterNoopRounds = getCollSizeMB(ns, st.shard0.rs.getPrimary());
const collSizeOnShard1AfterNoopRounds = getCollSizeMB(ns, st.shard1.rs.getPrimary());
const chunksAfterNoopRound = findChunksUtil.findChunksByNs(st.config, ns).toArray();
errMsg = '[AFTER NOOP ROUND] Data on shard0 = ' + collSizeOnShard0AfterNoopRounds +
    ' and data on shard 1 = ' + collSizeOnShard1AfterNoopRounds +
    ' - chunks before noop round = ' + JSON.stringify(chunksAfterNoopRound);
assert.eq(collSizeOnShard0BeforeNoopRounds, collSizeOnShard0AfterNoopRounds, errMsg);
assert.eq(collSizeOnShard1BeforeNoopRounds, collSizeOnShard1AfterNoopRounds, errMsg);
assert.eq(chunksBeforeNoopRound, chunksAfterNoopRound);

st.stop();
})();
