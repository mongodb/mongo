// Tests the behavior of change streams on sharded collections.
(function() {
    "use strict";

    load('jstests/replsets/libs/two_phase_drops.js');  // For TwoPhaseDropCollectionTest.
    load('jstests/aggregation/extras/utils.js');       // For assertErrorCode().
    load('jstests/libs/change_stream_util.js');        // For ChangeStreamTest.

    // For supportsMajorityReadConcern().
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    const st = new ShardingTest({
        shards: 2,
        rs: {
            nodes: 1,
            enableMajorityReadConcern: '',
            // Use a higher frequency for periodic noops to speed up the test.
            setParameter: {periodicNoopIntervalSecs: 1}
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

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey).
    assert.commandWorked(
        mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));

    // Move the [0, MaxKey) chunk to shard0001.
    assert.commandWorked(mongosDB.adminCommand(
        {moveChunk: mongosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));

    // Write a document to each chunk.
    assert.writeOK(mongosColl.insert({_id: -1}));
    assert.writeOK(mongosColl.insert({_id: 1}));

    let changeStream = mongosColl.aggregate([{$changeStream: {}}, {$project: {_id: 0}}]);

    // Test that a change stream can see inserts on shard 0.
    assert.writeOK(mongosColl.insert({_id: 1000}));
    assert.writeOK(mongosColl.insert({_id: -1000}));

    assert.soon(() => changeStream.hasNext(), "expected to be able to see the first insert");
    assert.docEq(changeStream.next(), {
        documentKey: {_id: 1000},
        fullDocument: {_id: 1000},
        ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
        operationType: "insert",
    });

    // Now do another write to shard 0, advancing that shard's clock and enabling the stream to
    // return the earlier write to shard 1.
    assert.writeOK(mongosColl.insert({_id: 1001}));

    assert.soon(() => changeStream.hasNext(), "expected to be able to see the second insert");
    assert.docEq(changeStream.next(), {
        documentKey: {_id: -1000},
        fullDocument: {_id: -1000},
        ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
        operationType: "insert",
    });

    // Test that all changes are eventually visible due to the periodic noop writer.
    assert.commandWorked(
        st.rs0.getPrimary().adminCommand({setParameter: 1, writePeriodicNoops: true}));
    assert.commandWorked(
        st.rs1.getPrimary().adminCommand({setParameter: 1, writePeriodicNoops: true}));
    assert.soon(() => changeStream.hasNext());

    assert.docEq(changeStream.next(), {
        documentKey: {_id: 1001},
        fullDocument: {_id: 1001},
        ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
        operationType: "insert",
    });
    changeStream.close();

    // Test that using change streams with any stages not allowed to run on mongos results in an
    // error.
    assertErrorCode(
        mongosColl, [{$changeStream: {}}, {$out: "shouldntWork"}], ErrorCodes.IllegalOperation);

    // Test that it is legal to open a change stream, even if the
    // 'internalQueryProhibitMergingOnMongos' parameter is set.
    assert.commandWorked(
        mongosDB.adminCommand({setParameter: 1, internalQueryProhibitMergingOnMongoS: true}));
    let tempCursor = assert.doesNotThrow(() => mongosColl.aggregate([{$changeStream: {}}]));
    tempCursor.close();
    assert.commandWorked(
        mongosDB.adminCommand({setParameter: 1, internalQueryProhibitMergingOnMongoS: false}));

    // Test that $sort and $group are banned from running in a $changeStream pipeline.
    assertErrorCode(mongosColl,
                    [{$changeStream: {}}, {$sort: {operationType: 1}}],
                    ErrorCodes.IllegalOperation);
    assertErrorCode(mongosColl,
                    [{$changeStream: {}}, {$group: {_id: "$documentKey"}}],
                    ErrorCodes.IllegalOperation);

    assert.writeOK(mongosColl.remove({}));
    // We awaited the replication of the first write, so the change stream shouldn't return it.
    // Use { w: "majority" } to deal with journaling correctly, even though we only have one node.
    assert.writeOK(mongosColl.insert({_id: 0, a: 1}, {writeConcern: {w: "majority"}}));

    changeStream = mongosColl.aggregate([{$changeStream: {}}, {$project: {"_id.clusterTime": 0}}]);
    assert(!changeStream.hasNext());

    // Drop the collection and test that we return "invalidate" entry and close the cursor.
    jsTestLog("Testing getMore command closes cursor for invalidate entries");
    mongosColl.drop();
    // Wait for the drop to actually happen.
    assert.soon(() => !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(
                    mongosColl.getDB(), mongosColl.getName()));
    assert.soon(() => changeStream.hasNext());
    assert.eq(changeStream.next().operationType, "invalidate");
    assert(!changeStream.hasNext(), "expected invalidation to cause the cursor to be closed");

    jsTestLog("Testing aggregate command closes cursor for invalidate entries");
    // Shard the test collection on _id.
    assert.commandWorked(
        mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey).
    assert.commandWorked(
        mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));
    // Move the [0, MaxKey) chunk to shard0001.
    assert.commandWorked(mongosDB.adminCommand(
        {moveChunk: mongosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));

    // Write one document to each chunk.
    assert.writeOK(mongosColl.insert({_id: -1}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({_id: 1}, {writeConcern: {w: "majority"}}));

    // Get a valid resume token that the next aggregate command can use.
    changeStream = mongosColl.aggregate([{$changeStream: {}}]);

    assert.writeOK(mongosColl.insert({_id: 2}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({_id: -2}, {writeConcern: {w: "majority"}}));

    assert.soon(() => changeStream.hasNext());
    const resumeToken = changeStream.next()._id;

    // It should not possible to resume a change stream after a collection drop, even if the
    // invalidate has not been received.
    assert(mongosColl.drop());
    // Wait for the drop to actually happen.
    assert.soon(() => !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(
                    mongosColl.getDB(), mongosColl.getName()));

    ChangeStreamTest.assertChangeStreamThrowsCode({
        collection: mongosColl,
        pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
        expectedCode: 40615
    });

})();
