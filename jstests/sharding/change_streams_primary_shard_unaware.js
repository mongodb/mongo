// Tests the behavior of a change stream on a collection that becomes sharded, however the primary
// shard is unaware and still sees the collection as unsharded.
//
// This test triggers a compiler bug that causes a crash when compiling with optimizations on, see
// SERVER-36321.
// @tags: [requires_persistence, blacklist_from_rhel_67_s390x, uses_change_streams]
(function() {
    "use strict";

    load('jstests/libs/change_stream_util.js');  // For ChangeStreamTest.

    // For supportsMajorityReadConcern().
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    // TODO (SERVER-38673): Remove this once BACKPORT-3428, BACKPORT-3429 are completed.
    if (!jsTestOptions().enableMajorityReadConcern &&
        jsTestOptions().merizosBinVersion === 'last-stable') {
        jsTestLog(
            "Skipping test since 'last-stable' merizos doesn't support speculative majority update lookup queries.");
        return;
    }

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
        merizos: 3,
        rs: {
            nodes: 1,
            // Use a higher frequency for periodic noops to speed up the test.
            setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true},
        },
    });

    const merizosDB = st.s0.getDB(testName);

    // Ensure that shard0 is the primary shard.
    assert.commandWorked(merizosDB.adminCommand({enableSharding: merizosDB.getName()}));
    st.ensurePrimaryShard(merizosDB.getName(), st.rs0.getURL());

    // Create unsharded collection on primary shard.
    const merizosColl = merizosDB[testName];
    assert.commandWorked(merizosDB.createCollection(testName));

    // Before sharding the collection, issue a write through merizos2 to ensure that it knows the
    // collection exists and believes it is unsharded. This is needed later in the test to avoid
    // triggering a refresh when a change stream is established through merizos2.
    const merizos2DB = st.s2.getDB(testName);
    const merizos2Coll = merizos2DB[testName];
    assert.writeOK(merizos2Coll.insert({_id: 0, a: 0}));

    // Create index on the shard key.
    assert.commandWorked(merizos2Coll.createIndex({a: 1}));

    // Shard the collection.
    assert.commandWorked(
        merizosDB.adminCommand({shardCollection: merizosColl.getFullName(), key: {a: 1}}));

    // Restart the primary shard and ensure that it is no longer aware that the collection is
    // sharded.
    st.restartShardRS(0);
    assert.eq(false, isShardAware(st.rs0.getPrimary(), merizosColl.getFullName()));

    const merizos1DB = st.s1.getDB(testName);
    const merizos1Coll = merizos1DB[testName];

    // Establish change stream cursor on the second merizos, which is not aware that the
    // collection is sharded.
    let cstMongos1 = new ChangeStreamTest(merizos1DB);
    let cursorMongos1 = cstMongos1.startWatchingChanges(
        {pipeline: [{$changeStream: {fullDocument: "updateLookup"}}], collection: merizos1Coll});
    assert.eq(0, cursorMongos1.firstBatch.length, "Cursor had changes: " + tojson(cursorMongos1));

    // Establish a change stream cursor on the now sharded collection through the first merizos.
    let cst = new ChangeStreamTest(merizosDB);
    let cursor = cst.startWatchingChanges(
        {pipeline: [{$changeStream: {fullDocument: "updateLookup"}}], collection: merizosColl});
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    // Ensure that the primary shard is still unaware that the collection is sharded.
    assert.eq(false, isShardAware(st.rs0.getPrimary(), merizosColl.getFullName()));

    // Insert a doc and verify that the primary shard is now aware that the collection is sharded.
    assert.writeOK(merizosColl.insert({_id: 1, a: 1}));
    assert.eq(true, isShardAware(st.rs0.getPrimary(), merizosColl.getFullName()));

    // Verify that both cursors are able to pick up an inserted document.
    cst.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: [{
            documentKey: {_id: 1, a: 1},
            fullDocument: {_id: 1, a: 1},
            ns: {db: merizosDB.getName(), coll: merizosColl.getName()},
            operationType: "insert",
        }]
    });
    let merizos1ChangeDoc = cstMongos1.getOneChange(cursorMongos1);
    assert.docEq({_id: 1, a: 1}, merizos1ChangeDoc.documentKey);
    assert.docEq({_id: 1, a: 1}, merizos1ChangeDoc.fullDocument);
    assert.eq({db: merizos1DB.getName(), coll: merizos1Coll.getName()}, merizos1ChangeDoc.ns);
    assert.eq("insert", merizos1ChangeDoc.operationType);

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey).
    assert.commandWorked(merizosDB.adminCommand({split: merizosColl.getFullName(), middle: {a: 0}}));

    // Move a chunk to the non-primary shard.
    assert.commandWorked(merizosDB.adminCommand({
        moveChunk: merizosColl.getFullName(),
        find: {a: -1},
        to: st.rs1.getURL(),
        _waitForDelete: true
    }));

    // Update the document on the primary shard.
    assert.writeOK(merizosColl.update({_id: 1, a: 1}, {$set: {b: 1}}));
    // Insert another document to each shard.
    assert.writeOK(merizosColl.insert({_id: -2, a: -2}));
    assert.writeOK(merizosColl.insert({_id: 2, a: 2}));

    // Verify that both cursors pick up the first inserted doc regardless of the moveChunk
    // operation.
    cst.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: [{
            documentKey: {_id: 1, a: 1},
            fullDocument: {_id: 1, a: 1, b: 1},
            ns: {db: merizosDB.getName(), coll: merizosColl.getName()},
            operationType: "update",
            updateDescription: {removedFields: [], updatedFields: {b: 1}}
        }]
    });
    merizos1ChangeDoc = cstMongos1.getOneChange(cursorMongos1);
    assert.docEq({_id: 1, a: 1}, merizos1ChangeDoc.documentKey);
    assert.docEq({_id: 1, a: 1, b: 1}, merizos1ChangeDoc.fullDocument);
    assert.eq({db: merizos1DB.getName(), coll: merizos1Coll.getName()}, merizos1ChangeDoc.ns);
    assert.eq("update", merizos1ChangeDoc.operationType);

    // Restart the primary shard and ensure that it is no longer aware that the collection is
    // sharded.
    st.restartShardRS(0);
    assert.eq(false, isShardAware(st.rs0.getPrimary(), merizosColl.getFullName()));

    // Establish change stream cursor on merizos2 using the resume token from the change steam on
    // merizos1. Mongos2 is aware that the collection exists and thinks that it's unsharded, so it
    // won't trigger a routing table refresh. This must be done using a resume token from an update
    // otherwise the shard will generate the documentKey based on the assumption that the shard key
    // is _id which will cause the cursor establishment to fail due to SERVER-32085.
    let cstMongos2 = new ChangeStreamTest(merizos2DB);
    let cursorMongos2 = cstMongos2.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: merizos1ChangeDoc._id}}],
        collection: merizos2Coll
    });

    cstMongos2.assertNextChangesEqual({
        cursor: cursorMongos2,
        expectedChanges: [{
            documentKey: {_id: -2, a: -2},
            fullDocument: {_id: -2, a: -2},
            ns: {db: merizos2DB.getName(), coll: merizos2Coll.getName()},
            operationType: "insert",
        }]
    });

    cstMongos2.assertNextChangesEqual({
        cursor: cursorMongos2,
        expectedChanges: [{
            documentKey: {_id: 2, a: 2},
            fullDocument: {_id: 2, a: 2},
            ns: {db: merizos2DB.getName(), coll: merizos2Coll.getName()},
            operationType: "insert",
        }]
    });

    st.stop();

})();
