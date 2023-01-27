/**
 * Tests that the analyzeShardKey command only returns the number of orphan documents if the
 * collection is sharded.
 *
 * @tags: [requires_fcv_63, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

function testAnalyzeShardKeyUnshardedCollection(conn) {
    const dbName = "testDb";
    const collName = "testCollUnsharded";
    const ns = dbName + "." + collName;
    const coll = conn.getCollection(ns);

    const candidateKey = {candidateKey: 1};
    assert.commandWorked(coll.createIndex(candidateKey));
    assert.commandWorked(coll.insert([{candidateKey: 1}]));

    const res = conn.adminCommand({analyzeShardKey: ns, key: candidateKey});
    assert.commandWorked(res);
    assert.eq(res.numDocs, 1, res);
    assert.eq(res.cardinality, 1, res);
    assert.eq(bsonWoCompare(res.frequency, {p99: 1, p95: 1, p90: 1, p80: 1, p50: 1}), 0, res);
    assert(!res.hasOwnProperty("numOrphanDocs"), res);

    assert(coll.drop());
}

function testAnalyzeShardKeyShardedCollection(st) {
    const dbName = "testDb";
    const collName = "testCollSharded";
    const ns = dbName + "." + collName;
    const coll = st.s.getCollection(ns);
    const currentKey = {currentKey: 1};
    const candidateKey = {candidateKey: 1};

    assert.commandWorked(coll.createIndex(currentKey));
    assert.commandWorked(coll.createIndex(candidateKey));
    assert.commandWorked(coll.insert([
        {currentKey: -10, candidateKey: -10},
        {currentKey: -5, candidateKey: -5},
        {currentKey: 0, candidateKey: 0},
        {currentKey: 5, candidateKey: 5},
        {currentKey: 10, candidateKey: 10}
    ]));

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: currentKey}));

    // Analyze a shard key while no shards have orphan documents. Chunk distribution:
    // shard0: [MinKey, 0]
    // shard1: [0, MaxKey]
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {currentKey: 0}}));
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: ns, find: {currentKey: 0}, to: st.shard1.shardName, _waitForDelete: true}));
    let res = st.s.adminCommand({analyzeShardKey: ns, key: candidateKey});
    assert.eq(res.numDocs, 5, res);
    assert.eq(res.cardinality, 5, res);
    assert.eq(bsonWoCompare(res.frequency, {p99: 1, p95: 1, p90: 1, p80: 1, p50: 1}), 0, res);
    assert(res.hasOwnProperty("numOrphanDocs"), res);
    assert.eq(res.numOrphanDocs, 0, res);

    // Pause range deletion on both shards.
    let suspendRangeDeletionFp0 = configureFailPoint(st.shard0, "suspendRangeDeletion");
    let suspendRangeDeletionFp1 = configureFailPoint(st.shard1, "suspendRangeDeletion");

    // Analyze a shard key while one shard has orphan documents. Chunk distribution:
    // shard0: [MinKey, -5]
    // shard1: [-5, 0], [0, MaxKey]
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {currentKey: -5}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {currentKey: -5}, to: st.shard1.shardName}));
    res = st.s.adminCommand({analyzeShardKey: ns, key: candidateKey});
    assert.eq(res.numDocs, 6, res);
    assert.eq(res.cardinality, 5, res);
    assert.eq(bsonWoCompare(res.frequency, {p99: 2, p95: 2, p90: 2, p80: 1, p50: 1}), 0, res);
    assert(res.hasOwnProperty("numOrphanDocs"), res);
    assert.eq(res.numOrphanDocs, 1, res);

    // Analyze a shard key while two shards have orphan documents. Chunk distribution:
    // shard0: [MinKey, -5], [5, MaxKey]
    // shard1: [-5, 0], [0, 5]
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {currentKey: 5}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {currentKey: 5}, to: st.shard0.shardName}));
    res = st.s.adminCommand({analyzeShardKey: ns, key: candidateKey});
    assert.eq(res.numDocs, 8, res);
    assert.eq(res.cardinality, 5, res);
    assert.eq(bsonWoCompare(res.frequency, {p99: 2, p95: 2, p90: 2, p80: 2, p50: 2}), 0, res);
    assert(res.hasOwnProperty("numOrphanDocs"), res);
    assert.eq(res.numOrphanDocs, 3, res);

    suspendRangeDeletionFp0.off();
    suspendRangeDeletionFp1.off();
    assert(coll.drop());
}

{
    const st = new ShardingTest({
        shards: 2,
        rs: {
            nodes: 2,
            setParameter: {
                "failpoint.analyzeShardKeySkipCalcalutingReadWriteDistributionMetrics":
                    tojson({mode: "alwaysOn"})
            }
        }
    });

    testAnalyzeShardKeyUnshardedCollection(st.s);
    testAnalyzeShardKeyShardedCollection(st);

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    testAnalyzeShardKeyUnshardedCollection(primary);

    rst.stopSet();
}
})();
