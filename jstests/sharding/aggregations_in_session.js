// Tests running aggregations within a client session. This test was designed to reproduce
// SERVER-33660.
(function() {
    "use strict";

    const st = new ShardingTest({shards: 2});

    // Gate this test to transaction supporting engines only as it uses txnNumber.
    let shardDB = st.rs0.getPrimary().getDB("test");
    if (!shardDB.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        jsTestLog("Do not run on storage engine that does not support transactions");
        st.stop();
        return;
    }

    const session = st.s0.getDB("test").getMongo().startSession();
    const merizosColl = session.getDatabase("test")[jsTestName()];

    // Shard the collection, split it into two chunks, and move the [1, MaxKey] chunk to the other
    // shard. We need chunks distributed across multiple shards in order to force a split pipeline
    // merging on a merizod - otherwise the entire pipeline will be forwarded without a split and
    // without a $mergeCursors stage.
    st.shardColl(merizosColl, {_id: 1}, {_id: 1}, {_id: 1});
    assert.writeOK(merizosColl.insert([{_id: 0}, {_id: 1}, {_id: 2}]));

    // This assertion will reproduce the hang described in SERVER-33660.
    assert.eq(
        [{_id: 0}, {_id: 1}, {_id: 2}],
        merizosColl
            .aggregate([{$_internalSplitPipeline: {mergeType: "primaryShard"}}, {$sort: {_id: 1}}])
            .toArray());

    // Test a couple more aggregations to be sure.
    assert.eq(
        [{_id: 0}, {_id: 1}, {_id: 2}],
        merizosColl.aggregate([{$_internalSplitPipeline: {mergeType: "merizos"}}, {$sort: {_id: 1}}])
            .toArray());
    assert.eq(merizosColl.aggregate([{$sort: {_id: 1}}, {$out: "testing"}]).itcount(), 0);

    assert.commandWorked(merizosColl.getDB().runCommand(
        {aggregate: merizosColl.getName(), pipeline: [], cursor: {}, txnNumber: NumberLong(1)}));

    st.stop();
}());
