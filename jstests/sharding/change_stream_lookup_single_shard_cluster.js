// Tests that a $changeStream pipeline is split rather than forwarded even in the case where the
// cluster only has a single shard, and that it can therefore successfully look up a document in a
// sharded collection.
(function() {
    "use strict";

    // For supportsMajorityReadConcern.
    load('jstests/multiVersion/libs/causal_consistency_helpers.js');

    // This test only works on storage engines that support committed reads, skip it if the
    // configured engine doesn't support it.
    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    // Create a cluster with only 1 shard.
    const st = new ShardingTest({
        shards: 1,
        rs: {nodes: 1, setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}}
    });

    const mongosDB = st.s0.getDB(jsTestName());
    const mongosColl = mongosDB[jsTestName()];

    // Enable sharding, shard on _id, and insert a test document which will be updated later.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    assert.commandWorked(
        mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));
    assert.writeOK(mongosColl.insert({_id: 1}));

    // Verify that the pipeline splits and merges on mongoS despite only targeting a single shard.
    const explainPlan = assert.commandWorked(
        mongosColl.explain().aggregate([{$changeStream: {fullDocument: "updateLookup"}}]));
    assert.neq(explainPlan.splitPipeline, null);
    assert.eq(explainPlan.mergeType, "mongos");

    // Open a $changeStream on the collection with 'updateLookup' and update the test doc.
    const stream = mongosColl.aggregate([{$changeStream: {fullDocument: "updateLookup"}}]);
    mongosColl.update({_id: 1}, {$set: {updated: true}});

    // Verify that the document is successfully retrieved from the sharded collection.
    assert.soon(() => stream.hasNext());
    assert.docEq(stream.next().fullDocument, {_id: 1, updated: true});

    stream.close();

    st.stop();
})();