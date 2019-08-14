// Tests the behavior of change streams on sharded collections.
// @tags: [uses_change_streams]
(function() {
"use strict";

load('jstests/replsets/libs/two_phase_drops.js');  // For TwoPhaseDropCollectionTest.
load('jstests/aggregation/extras/utils.js');       // For assertErrorCode().
load('jstests/libs/change_stream_util.js');        // For assertChangeStreamEventEq.

// For supportsMajorityReadConcern().
load("jstests/multiVersion/libs/causal_consistency_helpers.js");

if (!supportsMajorityReadConcern()) {
    jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
    return;
}

function runTest(collName, shardKey) {
    const st = new ShardingTest({
        shards: 2,
        rs: {
            nodes: 1,
            enableMajorityReadConcern: '',
            // Intentionally disable the periodic no-op writer in order to allow the test have
            // control of advancing the cluster time. For when it is enabled later in the test,
            // use a higher frequency for periodic noops to speed up the test.
            setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: false}
        }
    });

    const mongosDB = st.s0.getDB(jsTestName());
    assert.commandWorked(st.s0.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), st.shard0.shardName);

    const mongosColl = mongosDB[collName];

    //
    // Sanity tests
    //

    // Test that $sort and $group are banned from running in a $changeStream pipeline.
    assertErrorCode(mongosDB.NegativeTest,
                    [{$changeStream: {}}, {$sort: {operationType: 1}}],
                    ErrorCodes.IllegalOperation);
    assertErrorCode(mongosDB.NegativeTest,
                    [{$changeStream: {}}, {$group: {_id: '$documentKey'}}],
                    ErrorCodes.IllegalOperation);

    // Test that using change streams with $out results in an error.
    assertErrorCode(
        mongosColl, [{$changeStream: {}}, {$out: "shouldntWork"}], ErrorCodes.IllegalOperation);

    //
    // Main tests
    //

    function makeShardKey(value) {
        var obj = {};
        obj[shardKey] = value;
        return obj;
    }

    function makeShardKeyDocument(value, optExtraFields) {
        var obj = {};
        if (shardKey !== '_id')
            obj['_id'] = value;
        obj[shardKey] = value;
        return Object.assign(obj, optExtraFields);
    }

    jsTestLog('Testing change streams with shard key ' + shardKey);
    // Shard the test collection and split it into 2 chunks:
    //  [MinKey, 0) - shard0, [0, MaxKey) - shard1
    st.shardColl(mongosColl,
                 makeShardKey(1) /* shard key */,
                 makeShardKey(0) /* split at */,
                 makeShardKey(1) /* move to shard 1 */);

    // Write a document to each chunk.
    assert.commandWorked(mongosColl.insert(makeShardKeyDocument(-1)));
    assert.commandWorked(mongosColl.insert(makeShardKeyDocument(1)));

    let changeStream = mongosColl.aggregate([{$changeStream: {}}]);

    // Test that a change stream can see inserts on shard 0.
    assert.commandWorked(mongosColl.insert(makeShardKeyDocument(1000)));
    assert.commandWorked(mongosColl.insert(makeShardKeyDocument(-1000)));

    assert.soon(() => changeStream.hasNext(), "expected to be able to see the first insert");
    assertChangeStreamEventEq(changeStream.next(), {
        documentKey: makeShardKeyDocument(1000),
        fullDocument: makeShardKeyDocument(1000),
        ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
        operationType: "insert",
    });

    // Because the periodic noop writer is disabled, do another write to shard 0 in order to
    // advance that shard's clock and enabling the stream to return the earlier write to shard 1
    assert.commandWorked(mongosColl.insert(makeShardKeyDocument(1001)));

    assert.soon(() => changeStream.hasNext(), "expected to be able to see the second insert");
    assertChangeStreamEventEq(changeStream.next(), {
        documentKey: makeShardKeyDocument(-1000),
        fullDocument: makeShardKeyDocument(-1000),
        ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
        operationType: "insert",
    });

    // Test that all changes are eventually visible due to the periodic noop writer.
    assert.commandWorked(
        st.rs0.getPrimary().adminCommand({setParameter: 1, writePeriodicNoops: true}));
    assert.commandWorked(
        st.rs1.getPrimary().adminCommand({setParameter: 1, writePeriodicNoops: true}));

    assert.soon(() => changeStream.hasNext());
    assertChangeStreamEventEq(changeStream.next(), {
        documentKey: makeShardKeyDocument(1001),
        fullDocument: makeShardKeyDocument(1001),
        ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
        operationType: "insert",
    });
    changeStream.close();

    jsTestLog('Testing multi-update change streams with shard key ' + shardKey);
    assert.commandWorked(mongosColl.insert(makeShardKeyDocument(10, {a: 0, b: 0})));
    assert.commandWorked(mongosColl.insert(makeShardKeyDocument(-10, {a: 0, b: 0})));
    changeStream = mongosColl.aggregate([{$changeStream: {}}]);

    assert.commandWorked(mongosColl.update({a: 0}, {$set: {b: 2}}, {multi: true}));

    assert.soon(() => changeStream.hasNext());
    assertChangeStreamEventEq(changeStream.next(), {
        operationType: "update",
        ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
        documentKey: makeShardKeyDocument(-10),
        updateDescription: {updatedFields: {b: 2}, removedFields: []},
    });

    assert.soon(() => changeStream.hasNext());
    assertChangeStreamEventEq(changeStream.next(), {
        operationType: "update",
        ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
        documentKey: makeShardKeyDocument(10),
        updateDescription: {updatedFields: {b: 2}, removedFields: []},
    });
    changeStream.close();

    // Test that it is legal to open a change stream, even if the
    // 'internalQueryProhibitMergingOnMongos' parameter is set.
    assert.commandWorked(
        st.s0.adminCommand({setParameter: 1, internalQueryProhibitMergingOnMongoS: true}));
    let tempCursor = assert.doesNotThrow(() => mongosColl.aggregate([{$changeStream: {}}]));
    tempCursor.close();
    assert.commandWorked(
        st.s0.adminCommand({setParameter: 1, internalQueryProhibitMergingOnMongoS: false}));

    assert.commandWorked(mongosColl.remove({}));
    // We awaited the replication of the first write, so the change stream shouldn't return it.
    // Use { w: "majority" } to deal with journaling correctly, even though we only have one
    // node.
    assert.commandWorked(
        mongosColl.insert(makeShardKeyDocument(0, {a: 1}), {writeConcern: {w: "majority"}}));

    changeStream = mongosColl.aggregate([{$changeStream: {}}]);
    assert(!changeStream.hasNext());

    // Drop the collection and test that we return a "drop" followed by an "invalidate" entry
    // and close the cursor.
    jsTestLog('Testing getMore command closes cursor for invalidate entries with shard key' +
              shardKey);
    mongosColl.drop();
    // Wait for the drop to actually happen.
    assert.soon(() => !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(
                    mongosColl.getDB(), mongosColl.getName()));
    assert.soon(() => changeStream.hasNext());
    assert.eq(changeStream.next().operationType, "drop");
    assert.soon(() => changeStream.hasNext());
    assert.eq(changeStream.next().operationType, "invalidate");
    assert(changeStream.isExhausted());

    jsTestLog('Testing aggregate command closes cursor for invalidate entries with shard key' +
              shardKey);
    // Shard the test collection and split it into 2 chunks:
    //  [MinKey, 0) - shard0, [0, MaxKey) - shard1
    st.shardColl(mongosColl,
                 makeShardKey(1) /* shard key */,
                 makeShardKey(0) /* split at */,
                 makeShardKey(1) /* move to shard 1 */);

    // Write one document to each chunk.
    assert.commandWorked(
        mongosColl.insert(makeShardKeyDocument(-1), {writeConcern: {w: "majority"}}));
    assert.commandWorked(
        mongosColl.insert(makeShardKeyDocument(1), {writeConcern: {w: "majority"}}));

    changeStream = mongosColl.aggregate([{$changeStream: {}}]);
    assert(!changeStream.hasNext());

    // Store a valid resume token before dropping the collection, to be used later in the test
    assert.commandWorked(
        mongosColl.insert(makeShardKeyDocument(-2), {writeConcern: {w: "majority"}}));
    assert.commandWorked(
        mongosColl.insert(makeShardKeyDocument(2), {writeConcern: {w: "majority"}}));

    assert.soon(() => changeStream.hasNext());
    const resumeToken = changeStream.next()._id;

    mongosColl.drop();

    assert.soon(() => changeStream.hasNext());
    assertChangeStreamEventEq(changeStream.next(), {
        documentKey: makeShardKeyDocument(2),
        fullDocument: makeShardKeyDocument(2),
        ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
        operationType: "insert",
    });

    assert.soon(() => changeStream.hasNext());
    assert.eq(changeStream.next().operationType, "drop");

    assert.soon(() => changeStream.hasNext());
    assert.eq(changeStream.next().operationType, "invalidate");

    // With an explicit collation, test that we can resume from before the collection drop
    changeStream = mongosColl.watch([], {resumeAfter: resumeToken, collation: {locale: "simple"}});

    assert.soon(() => changeStream.hasNext());
    assertChangeStreamEventEq(changeStream.next(), {
        documentKey: makeShardKeyDocument(2),
        fullDocument: makeShardKeyDocument(2),
        ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
        operationType: "insert",
    });

    assert.soon(() => changeStream.hasNext());
    assert.eq(changeStream.next().operationType, "drop");

    assert.soon(() => changeStream.hasNext());
    assert.eq(changeStream.next().operationType, "invalidate");

    // Test that we can resume from before the collection drop without an explicit collation.
    assert.commandWorked(mongosDB.runCommand({
        aggregate: mongosColl.getName(),
        pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
        cursor: {}
    }));

    st.stop();
}

runTest('with_id_shard_key', '_id');
runTest('with_non_id_shard_key', 'non_id');
})();
