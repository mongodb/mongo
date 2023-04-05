/**
 * Tests that the analyzeShardKey command correctly returns the average document size in bytes.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

function testUnshardedCollection(conn) {
    const dbName = "testDb";
    const collName = "testCollUnsharded";
    const ns = dbName + "." + collName;
    const coll = conn.getCollection(ns);

    const candidateKey = {candidateKey: 1};
    assert.commandWorked(coll.createIndex(candidateKey));
    assert.commandWorked(
        coll.insert([{candidateKey: "a"}, {candidateKey: new Array(1000).join("a")}]));

    const res = assert.commandWorked(conn.adminCommand({analyzeShardKey: ns, key: candidateKey}));
    assert.lt(res.avgDocSizeBytes, 1000, res);
    assert.gt(res.avgDocSizeBytes, 1000 / 2, res);

    assert(coll.drop());
}

function testShardedCollection(st) {
    const dbName = "testDb";
    const collName = "testCollSharded";
    const ns = dbName + "." + collName;
    const coll = st.s.getCollection(ns);
    const currentKey = {currentKey: 1};
    const candidateKey = {candidateKey: 1};

    assert.commandWorked(coll.createIndex(currentKey));
    assert.commandWorked(coll.createIndex(candidateKey));

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: currentKey}));

    // Make the collection have the following chunks:
    // shard0: [MinKey, 0]
    // shard1: [0, MaxKey]
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {currentKey: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {currentKey: 0}, to: st.shard1.shardName}));

    assert.commandWorked(coll.insert([
        {currentKey: -10, candidateKey: "a"},
        {currentKey: -5, candidateKey: "a"},
        {currentKey: 0, candidateKey: new Array(1000).join("a")},
        {currentKey: 5, candidateKey: new Array(1000).join("a")},
        {currentKey: 10, candidateKey: new Array(1000).join("a")}
    ]));

    const res = st.s.adminCommand({analyzeShardKey: ns, key: candidateKey});
    assert.lt(res.avgDocSizeBytes, 1000, res);
    assert.gt(res.avgDocSizeBytes, 3000 / 5, res);

    assert(coll.drop());
}

const setParameterOpts = {
    // Skip calculating the read and write distribution metrics since there are no sampled queries
    // anyway.
    "failpoint.analyzeShardKeySkipCalcalutingReadWriteDistributionMetrics":
        tojson({mode: "alwaysOn"})
};

{
    const st = new ShardingTest({shards: 2, rs: {nodes: 2, setParameter: setParameterOpts}});

    testUnshardedCollection(st.s);
    testShardedCollection(st);

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: setParameterOpts}});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    testUnshardedCollection(primary);

    rst.stopSet();
}
})();
