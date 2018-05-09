// Tests invalidation of change streams on sharded collections.
(function() {
    "use strict";

    load('jstests/replsets/libs/two_phase_drops.js');  // For TwoPhaseDropCollectionTest.
    load('jstests/libs/write_concern_util.js');        // For stopReplicationOnSecondaries.

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

    // Drop the collection and test that we return "invalidate" entry and close the cursor.
    mongosColl.drop();

    // Test that we see the two writes that happened before the invalidation.
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
    assert.eq(next.operationType, "invalidate");
    assert(changeStream.isExhausted());

    // Test that it is not possible to resume a change stream after a collection has been dropped.
    // Once it's been dropped, we won't be able to figure out the shard key.
    assert.soon(() => !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(
                    mongosColl.getDB(), mongosColl.getName()));
    assert.commandFailedWithCode(mongosDB.runCommand({
        aggregate: mongosColl.getName(),
        pipeline: [{$changeStream: {resumeAfter: resumeTokenFromFirstUpdate}}],
        cursor: {}
    }),
                                 40615);

    st.stop();
})();
