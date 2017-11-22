// Tests that resuming a change stream that has become sharded via a mongos that believes the
// collection is still unsharded will end up targeting the change stream to all shards after getting
// a stale shard version.
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
        mongos: 2,
        rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
    });

    const firstMongosDB = st.s0.getDB(jsTestName());
    const firstMongosColl = firstMongosDB.test;

    // Enable sharding on the test DB and ensure its primary is shard 0.
    assert.commandWorked(firstMongosDB.adminCommand({enableSharding: firstMongosDB.getName()}));
    st.ensurePrimaryShard(firstMongosDB.getName(), st.rs0.getURL());

    // Establish a change stream while it is unsharded, then shard the collection, move a chunk, and
    // record a resume token after the first chunk migration.
    let changeStream = firstMongosColl.aggregate([{$changeStream: {}}]);

    assert.writeOK(firstMongosColl.insert({_id: -1}));
    assert.writeOK(firstMongosColl.insert({_id: 1}));

    for (let nextId of[-1, 1]) {
        assert.soon(() => changeStream.hasNext());
        let next = changeStream.next();
        assert.eq(next.operationType, "insert");
        assert.eq(next.fullDocument, {_id: nextId});
    }

    // Shard the test collection on _id, split the collection into 2 chunks: [MinKey, 0) and
    // [0, MaxKey), then move the [0, MaxKey) chunk to shard 1.
    assert.commandWorked(firstMongosDB.adminCommand(
        {shardCollection: firstMongosColl.getFullName(), key: {_id: 1}}));
    assert.commandWorked(
        firstMongosDB.adminCommand({split: firstMongosColl.getFullName(), middle: {_id: 0}}));
    assert.commandWorked(firstMongosDB.adminCommand(
        {moveChunk: firstMongosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));

    // Then do one insert to each shard.
    assert.writeOK(firstMongosColl.insert({_id: -2}));
    assert.writeOK(firstMongosColl.insert({_id: 2}));

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
    assert.writeOK(firstMongosColl.insert({_id: -3}));
    assert.writeOK(firstMongosColl.insert({_id: 3}));

    // Now try to resume the change stream using a stale mongos which believes the collection is
    // unsharded. The first mongos should use the shard versioning protocol to discover that the
    // collection is no longer unsharded, and re-target to all shards in the cluster.
    changeStream.close();
    const secondMongosColl = st.s1.getDB(jsTestName()).test;
    changeStream = secondMongosColl.aggregate([{$changeStream: {resumeAfter: resumeToken}}]);
    // Verify we can see both inserts that occurred after the resume point.
    for (let nextId of[-3, 3]) {
        assert.soon(() => changeStream.hasNext());
        let next = changeStream.next();
        assert.eq(next.operationType, "insert");
        assert.eq(next.fullDocument, {_id: nextId});
    }

    st.stop();
}());
