// Tests that a $changeStream pipeline is split rather than forwarded even in the case where the
// cluster only has a single shard, and that it can therefore successfully look up a document in a
// sharded collection.
// @tags: [uses_change_streams]
(function() {
    "use strict";

    // For supportsMajorityReadConcern.
    load('jstests/multiVersion/libs/causal_consistency_helpers.js');

    // TODO (SERVER-38673): Remove this once BACKPORT-3428, BACKPORT-3429 are completed.
    if (!jsTestOptions().enableMajorityReadConcern &&
        jsTestOptions().merizosBinVersion === 'last-stable') {
        jsTestLog(
            "Skipping test since 'last-stable' merizos doesn't support speculative majority update lookup queries.");
        return;
    }

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

    const merizosDB = st.s0.getDB(jsTestName());
    const merizosColl = merizosDB[jsTestName()];

    // Enable sharding, shard on _id, and insert a test document which will be updated later.
    assert.commandWorked(merizosDB.adminCommand({enableSharding: merizosDB.getName()}));
    assert.commandWorked(
        merizosDB.adminCommand({shardCollection: merizosColl.getFullName(), key: {_id: 1}}));
    assert.writeOK(merizosColl.insert({_id: 1}));

    // Verify that the pipeline splits and merges on merizoS despite only targeting a single shard.
    const explainPlan = assert.commandWorked(
        merizosColl.explain().aggregate([{$changeStream: {fullDocument: "updateLookup"}}]));
    assert.neq(explainPlan.splitPipeline, null);
    assert.eq(explainPlan.mergeType, "merizos");

    // Open a $changeStream on the collection with 'updateLookup' and update the test doc.
    const stream = merizosColl.watch([], {fullDocument: "updateLookup"});
    const wholeDbStream = merizosDB.watch([], {fullDocument: "updateLookup"});

    merizosColl.update({_id: 1}, {$set: {updated: true}});

    // Verify that the document is successfully retrieved from the single-collection and whole-db
    // change streams.
    assert.soon(() => stream.hasNext());
    assert.docEq(stream.next().fullDocument, {_id: 1, updated: true});

    assert.soon(() => wholeDbStream.hasNext());
    assert.docEq(wholeDbStream.next().fullDocument, {_id: 1, updated: true});

    stream.close();
    wholeDbStream.close();

    st.stop();
})();
