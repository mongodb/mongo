// Tests that resuming a change stream that has become sharded via a merizos that believes the
// collection is still unsharded will end up targeting the change stream to all shards after getting
// a stale shard version.
// @tags: [uses_change_streams]
(function() {
    "use strict";

    // For supportsMajorityReadConcern().
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    // Create a 2-shard cluster. Enable 'writePeriodicNoops' and set 'periodicNoopIntervalSecs' to 1
    // second so that each shard is continually advancing its optime, allowing the
    // AsyncResultsMerger to return sorted results even if some shards have not yet produced any
    // data.
    const st = new ShardingTest({
        shards: 2,
        merizos: 2,
        rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
    });

    const firstMerizosDB = st.s0.getDB(jsTestName());
    const firstMerizosColl = firstMerizosDB.test;

    // Enable sharding on the test DB and ensure its primary is shard 0.
    assert.commandWorked(firstMerizosDB.adminCommand({enableSharding: firstMerizosDB.getName()}));
    st.ensurePrimaryShard(firstMerizosDB.getName(), st.rs0.getURL());

    // Establish a change stream while it is unsharded, then shard the collection, move a chunk, and
    // record a resume token after the first chunk migration.
    let changeStream = firstMerizosColl.aggregate([{$changeStream: {}}]);

    assert.writeOK(firstMerizosColl.insert({_id: -1}));
    assert.writeOK(firstMerizosColl.insert({_id: 1}));

    for (let nextId of[-1, 1]) {
        assert.soon(() => changeStream.hasNext());
        let next = changeStream.next();
        assert.eq(next.operationType, "insert");
        assert.eq(next.fullDocument, {_id: nextId});
    }

    // Shard the test collection on _id, split the collection into 2 chunks: [MinKey, 0) and
    // [0, MaxKey), then move the [0, MaxKey) chunk to shard 1.
    assert.commandWorked(firstMerizosDB.adminCommand(
        {shardCollection: firstMerizosColl.getFullName(), key: {_id: 1}}));
    assert.commandWorked(
        firstMerizosDB.adminCommand({split: firstMerizosColl.getFullName(), middle: {_id: 0}}));
    assert.commandWorked(firstMerizosDB.adminCommand(
        {moveChunk: firstMerizosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));

    // Then do one insert to each shard.
    assert.writeOK(firstMerizosColl.insert({_id: -2}));
    assert.writeOK(firstMerizosColl.insert({_id: 2}));

    // The change stream should see all the inserts after internally re-establishing cursors after
    // the chunk split.
    let resumeToken = null;  // We'll fill this out to be the token of the last change.
    for (let nextId of[-2, 2]) {
        assert.soon(() => changeStream.hasNext());
        let next = changeStream.next();
        assert.eq(next.operationType, "insert");
        assert.eq(next.fullDocument, {_id: nextId});
        resumeToken = next._id;
    }

    // Do some writes that occur on each shard after the resume token.
    assert.writeOK(firstMerizosColl.insert({_id: -3}));
    assert.writeOK(firstMerizosColl.insert({_id: 3}));

    // Now try to resume the change stream using a stale merizos which believes the collection is
    // unsharded. The first merizos should use the shard versioning protocol to discover that the
    // collection is no longer unsharded, and re-target to all shards in the cluster.
    changeStream.close();
    const secondMerizosColl = st.s1.getDB(jsTestName()).test;
    changeStream = secondMerizosColl.aggregate([{$changeStream: {resumeAfter: resumeToken}}]);
    // Verify we can see both inserts that occurred after the resume point.
    for (let nextId of[-3, 3]) {
        assert.soon(() => changeStream.hasNext());
        let next = changeStream.next();
        assert.eq(next.operationType, "insert");
        assert.eq(next.fullDocument, {_id: nextId});
    }

    st.stop();
}());
