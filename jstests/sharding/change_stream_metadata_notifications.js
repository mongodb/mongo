// Tests metadata notifications of change streams on sharded collections.
// Legacy getMore fails after dropping the database that the original cursor is on.
// @tags: [requires_find_command]
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.
    load('jstests/replsets/libs/two_phase_drops.js');  // For TwoPhaseDropCollectionTest.

    // For supportsMajorityReadConcern.
    load('jstests/multiVersion/libs/causal_consistency_helpers.js');

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    const st = new ShardingTest({
        shards: 2,
        rs: {
            nodes: 1,
            enableMajorityReadConcern: '',
        }
    });

    const mongosDB = st.s0.getDB(jsTestName());
    const mongosColl = mongosDB[jsTestName()];

    assert.commandWorked(mongosDB.dropDatabase());

    // Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

    // Shard the test collection on a field called 'shardKey'.
    assert.commandWorked(
        mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {shardKey: 1}}));

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey].
    assert.commandWorked(
        mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {shardKey: 0}}));

    // Move the [0, MaxKey] chunk to st.shard1.shardName.
    assert.commandWorked(mongosDB.adminCommand(
        {moveChunk: mongosColl.getFullName(), find: {shardKey: 1}, to: st.rs1.getURL()}));

    // Write a document to each chunk.
    assert.writeOK(mongosColl.insert({shardKey: -1, _id: -1}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({shardKey: 1, _id: 1}, {writeConcern: {w: "majority"}}));

    let changeStream = mongosColl.watch();

    // We awaited the replication of the first writes, so the change stream shouldn't return them.
    assert.writeOK(mongosColl.update({shardKey: -1, _id: -1}, {$set: {updated: true}}));
    assert.writeOK(mongosColl.update({shardKey: 1, _id: 1}, {$set: {updated: true}}));
    assert.writeOK(mongosColl.insert({shardKey: 2, _id: 2}));

    // Drop the collection and test that we return a "drop" entry, followed by an "invalidate"
    // entry.
    mongosColl.drop();

    // Test that we see the two writes that happened before the collection drop.
    assert.soon(() => changeStream.hasNext());
    let next = changeStream.next();
    assert.eq(next.operationType, "update");
    assert.eq(next.documentKey.shardKey, -1);
    const resumeTokenFromFirstUpdate = next._id;

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "update");
    assert.eq(next.documentKey.shardKey, 1);

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "insert");
    assert.eq(next.documentKey, {_id: 2});

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "drop");
    assert.eq(next.ns, {db: mongosDB.getName(), coll: mongosColl.getName()});

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "invalidate");
    assert(changeStream.isExhausted());

    // With an explicit collation, test that we can resume from before the collection drop.
    changeStream =
        mongosColl.watch([{$project: {_id: 0}}],
                         {resumeAfter: resumeTokenFromFirstUpdate, collation: {locale: "simple"}});

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "update");
    assert.eq(next.documentKey, {shardKey: 1, _id: 1});

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "insert");
    assert.eq(next.documentKey, {shardKey: 2, _id: 2});

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "drop");
    assert.eq(next.ns, {db: mongosDB.getName(), coll: mongosColl.getName()});

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "invalidate");
    assert(changeStream.isExhausted());

    // Test that we cannot resume the change stream without specifying an explicit collation.
    assert.commandFailedWithCode(mongosDB.runCommand({
        aggregate: mongosColl.getName(),
        pipeline: [{$changeStream: {resumeAfter: resumeTokenFromFirstUpdate}}],
        cursor: {}
    }),
                                 ErrorCodes.InvalidResumeToken);

    // Recreate and shard the collection.
    assert.commandWorked(mongosDB.createCollection(mongosColl.getName()));

    // Shard the test collection on shardKey.
    assert.commandWorked(
        mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {shardKey: 1}}));

    // Test that resuming the change stream on the recreated collection fails since the UUID has
    // changed.
    assert.commandFailedWithCode(mongosDB.runCommand({
        aggregate: mongosColl.getName(),
        pipeline: [{$changeStream: {resumeAfter: resumeTokenFromFirstUpdate}}],
        cursor: {}
    }),
                                 ErrorCodes.InvalidResumeToken);

    // Recreate the collection as unsharded and open a change stream on it.
    assertDropAndRecreateCollection(mongosDB, mongosColl.getName());

    changeStream = mongosColl.watch();

    // Drop the database and verify that the stream returns a collection drop followed by an
    // invalidate.
    assert.commandWorked(mongosDB.dropDatabase());

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "drop");
    assert.eq(next.ns, {db: mongosDB.getName(), coll: mongosColl.getName()});

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "invalidate");
    assert(changeStream.isExhausted());

    st.stop();
})();
