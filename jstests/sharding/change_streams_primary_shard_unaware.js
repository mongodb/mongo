// Tests the behavior of a change stream on a collection that becomes sharded, however the primary
// shard is unaware and still sees the collection as unsharded.
//
// @tags: [requires_persistence]
(function() {
    "use strict";

    load('jstests/libs/change_stream_util.js');  // For ChangeStreamTest.

    // For supportsMajorityReadConcern().
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    // Returns true if the shard is aware that the collection is sharded.
    function isShardAware(shard, coll) {
        const res = shard.adminCommand({getShardVersion: coll, fullMetadata: true});
        assert.commandWorked(res);
        return res.metadata.collVersion != undefined;
    }

    const testName = "change_streams_primary_shard_unaware";
    const st = new ShardingTest({
        shards: 2,
        mongos: 3,
        rs: {
            nodes: 1,
            // Use a higher frequency for periodic noops to speed up the test.
            setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true},
        },
    });

    const mongosDB = st.s0.getDB(testName);

    // Ensure that shard0 is the primary shard.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

    // Create unsharded collection on primary shard.
    const mongosColl = mongosDB[testName];
    assert.commandWorked(mongosDB.createCollection(testName));

    // Before sharding the collection, issue a write through mongos2 to ensure that it knows the
    // collection exists and believes it is unsharded. This is needed later in the test to avoid
    // triggering a refresh when a change stream is established through mongos2.
    const mongos2DB = st.s2.getDB(testName);
    const mongos2Coll = mongos2DB[testName];
    assert.writeOK(mongos2Coll.insert({_id: 0, a: 0}));

    // Create index on the shard key.
    assert.commandWorked(mongos2Coll.createIndex({a: 1}));

    // Shard the collection.
    assert.commandWorked(
        mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {a: 1}}));

    // Restart the primary shard and ensure that it is no longer aware that the collection is
    // sharded.
    st.rs0.restart(0);
    assert.eq(false, isShardAware(st.rs0.getPrimary(), mongosColl.getFullName()));

    const mongos1DB = st.s1.getDB(testName);
    const mongos1Coll = mongos1DB[testName];

    // Establish change stream cursor on the second mongos, which is not aware that the
    // collection is sharded.
    let cstMongos1 = new ChangeStreamTest(mongos1DB);
    let cursorMongos1 = cstMongos1.startWatchingChanges({
        pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
        collection: mongos1Coll,
        includeToken: true
    });
    assert.eq(0, cursorMongos1.firstBatch.length, "Cursor had changes: " + tojson(cursorMongos1));

    // Establish a change stream cursor on the now sharded collection through the first mongos.
    let cst = new ChangeStreamTest(mongosDB);
    let cursor = cst.startWatchingChanges(
        {pipeline: [{$changeStream: {fullDocument: "updateLookup"}}], collection: mongosColl});
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    // Ensure that the primary shard is still unaware that the collection is sharded.
    assert.eq(false, isShardAware(st.rs0.getPrimary(), mongosColl.getFullName()));

    // Insert a doc and verify that the primary shard is now aware that the collection is sharded.
    assert.writeOK(mongosColl.insert({_id: 1, a: 1}));
    assert.eq(true, isShardAware(st.rs0.getPrimary(), mongosColl.getFullName()));

    // Verify that both cursors are able to pick up an inserted document.
    cst.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: [{
            documentKey: {_id: 1, a: 1},
            fullDocument: {_id: 1, a: 1},
            ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
            operationType: "insert",
        }]
    });
    let mongos1ChangeDoc = cstMongos1.getOneChange(cursorMongos1);
    assert.docEq({_id: 1, a: 1}, mongos1ChangeDoc.documentKey);
    assert.docEq({_id: 1, a: 1}, mongos1ChangeDoc.fullDocument);
    assert.eq({db: mongos1DB.getName(), coll: mongos1Coll.getName()}, mongos1ChangeDoc.ns);
    assert.eq("insert", mongos1ChangeDoc.operationType);

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey).
    assert.commandWorked(mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {a: 0}}));

    // Move a chunk to the non-primary shard.
    assert.commandWorked(mongosDB.adminCommand({
        moveChunk: mongosColl.getFullName(),
        find: {a: -1},
        to: st.rs1.getURL(),
        _waitForDelete: true
    }));

    // Update the document on the primary shard.
    assert.writeOK(mongosColl.update({_id: 1, a: 1}, {$set: {b: 1}}));
    // Insert another document to each shard.
    assert.writeOK(mongosColl.insert({_id: -2, a: -2}));
    assert.writeOK(mongosColl.insert({_id: 2, a: 2}));

    // Verify that both cursors pick up the first inserted doc regardless of the moveChunk
    // operation.
    cst.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: [{
            documentKey: {_id: 1, a: 1},
            fullDocument: {_id: 1, a: 1, b: 1},
            ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
            operationType: "update",
            updateDescription: {removedFields: [], updatedFields: {b: 1}}
        }]
    });
    mongos1ChangeDoc = cstMongos1.getOneChange(cursorMongos1);
    assert.docEq({_id: 1, a: 1}, mongos1ChangeDoc.documentKey);
    assert.docEq({_id: 1, a: 1, b: 1}, mongos1ChangeDoc.fullDocument);
    assert.eq({db: mongos1DB.getName(), coll: mongos1Coll.getName()}, mongos1ChangeDoc.ns);
    assert.eq("update", mongos1ChangeDoc.operationType);

    // Restart the primary shard and ensure that it is no longer aware that the collection is
    // sharded.
    st.rs0.restart(0);
    assert.eq(false, isShardAware(st.rs0.getPrimary(), mongosColl.getFullName()));

    // Establish change stream cursor on mongos2 using the resume token from the change steam on
    // mongos1. Mongos2 is aware that the collection exists and thinks that it's unsharded, so it
    // won't trigger a routing table refresh. This must be done using a resume token from an update
    // otherwise the shard will generate the documentKey based on the assumption that the shard key
    // is _id which will cause the cursor establishment to fail due to SERVER-32085.
    let cstMongos2 = new ChangeStreamTest(mongos2DB);
    let cursorMongos2 = cstMongos2.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: mongos1ChangeDoc._id}}],
        collection: mongos2Coll
    });

    // Since neither the mongos nor the shard are aware that the collection is sharded, the change
    // stream will miss the inserted document to the other shard.
    // TODO: After SERVER-32198, the expected behavior is that the change stream will correctly
    // resume since establishing the cursor on the restarted shard will trigger a refresh.
    // cstMongos2.assertNextChangesEqual({
    //     cursor: cursorMongos2,
    //     expectedChanges: [{
    //         documentKey: {_id: -2, a: -2},
    //         fullDocument: {_id: -2, a: -2},
    //         ns: {db: mongos2DB.getName(), coll: mongos2Coll.getName()},
    //         operationType: "insert",
    //     }]
    // });
    cstMongos2.assertNextChangesEqual({
        cursor: cursorMongos2,
        expectedChanges: [{
            documentKey: {_id: 2},
            fullDocument: {_id: 2, a: 2},
            ns: {db: mongos2DB.getName(), coll: mongos2Coll.getName()},
            operationType: "insert",
        }]
    });

    st.stop();

})();
