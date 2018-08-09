// Tests invalidation of change streams on sharded collections.
// @tags: [requires_majority_read_concern]
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

    // Enable sharding on the test DB and ensure its primary is shard0000.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

    // Shard the test collection on _id.
    assert.commandWorked(
        mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey].
    assert.commandWorked(
        mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));

    // Move the [0, MaxKey] chunk to shard0001.
    assert.commandWorked(mongosDB.adminCommand(
        {moveChunk: mongosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));

    // Write a document to each chunk.
    assert.writeOK(mongosColl.insert({_id: -1}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({_id: 1}, {writeConcern: {w: "majority"}}));

    let changeStream = mongosColl.aggregate([{$changeStream: {}}]);

    // We awaited the replication of the first writes, so the change stream shouldn't return them.
    assert.writeOK(mongosColl.update({_id: -1}, {$set: {updated: true}}));
    assert.writeOK(mongosColl.update({_id: 1}, {$set: {updated: true}}));

    // Drop the collection and test that we return "invalidate" entry and close the cursor.
    mongosColl.drop();
    st.rs0.awaitReplication();
    st.rs1.awaitReplication();

    // Test that we see the two writes that happened before the invalidation.
    assert.soon(() => changeStream.hasNext());
    let next = changeStream.next();
    assert.eq(next.operationType, "update");
    assert.eq(next.documentKey._id, -1);
    const resumeTokenFromFirstUpdate = next._id;

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "update");
    assert.eq(next.documentKey._id, 1);

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "invalidate");

    assert(!changeStream.hasNext(), "expected invalidation to cause the cursor to be closed");

    // Test that it is not possible to resume a change stream after a collection has been dropped.
    // Once it's been dropped, we won't be able to figure out the shard key.
    assert.soon(() => !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(
                    mongosColl.getDB(), mongosColl.getName()));
    assert.commandFailedWithCode(mongosDB.runCommand({
        aggregate: mongosColl.getName(),
        pipeline: [{$changeStream: {resumeAfter: resumeTokenFromFirstUpdate}}],
        readConcern: {level: "majority"},
        cursor: {}
    }),
                                 40615);

    st.stop();
})();
