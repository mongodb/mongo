// Tests the behavior of change streams on a collection that was initially unsharded but then
// becomes sharded.
(function() {
    "use strict";

    load('jstests/libs/change_stream_util.js');  // For ChangeStreamTest.

    // For supportsMajorityReadConcern().
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    const testName = "change_streams_unsharded_becomes_sharded";
    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        rs: {
            nodes: 1,
            enableMajorityReadConcern: '',
            // Use a higher frequency for periodic noops to speed up the test.
            setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}
        }
    });

    const mongosDB = st.s0.getDB(testName);
    const mongosColl = mongosDB[testName];

    mongosDB.createCollection(testName);

    st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

    // Establish a change stream cursor on the unsharded collection.
    let cst = new ChangeStreamTest(mongosDB);
    let cursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: mongosColl});
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    // Enable sharding on the previously unsharded collection.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));

    // Shard the collection on _id.
    assert.commandWorked(
        mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey).
    assert.commandWorked(
        mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));

    // Verify that the cursor is still valid and picks up the inserted document.
    assert.writeOK(mongosColl.insert({_id: 1}));
    cst.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: [{
            documentKey: {_id: 1},
            fullDocument: {_id: 1},
            ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
            operationType: "insert",
        }]
    });

    // Move the [minKey, 0) chunk to shard1 and write a document to it.
    assert.commandWorked(mongosDB.adminCommand({
        moveChunk: mongosColl.getFullName(),
        find: {_id: -1},
        to: st.rs1.getURL(),
        _waitForDelete: true
    }));
    assert.writeOK(mongosColl.insert({_id: -1}));

    // Since a moveChunk was requested, the cursor results should indicate a retry is needed.
    // TODO: SERVER-30834 this result will get swallowed and the change stream cursor should see
    // the inserted document.
    cst.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: [{
            operationType: "retryNeeded",
        }]
    });

    st.stop();
})();
