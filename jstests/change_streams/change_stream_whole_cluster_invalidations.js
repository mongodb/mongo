// Tests of invalidate entries for a $changeStream on a whole cluster.
(function() {
    "use strict";

    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.
    load('jstests/replsets/libs/two_phase_drops.js');  // For 'TwoPhaseDropCollectionTest'.
    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.

    // Define two databases. We will conduct our tests by creating one collection in each.
    const testDB1 = db, testDB2 = db.getSiblingDB(`${db.getName()}_other`);
    const adminDB = db.getSiblingDB("admin");

    // Create one collection on each database.
    let [db1Coll, db2Coll] =
        [testDB1, testDB2].map((testDB) => assertDropAndRecreateCollection(testDB, jsTestName()));

    // Create a ChangeStreamTest on the 'admin' db. Cluster-wide change streams can only be opened
    // on admin.
    let cst = new ChangeStreamTest(adminDB);
    let aggCursor = cst.startWatchingAllChangesForCluster();

    //  Generate oplog entries of type insert, update, and delete across both databases.
    for (let coll of[db1Coll, db2Coll]) {
        assert.writeOK(coll.insert({_id: 1}));
        assert.writeOK(coll.update({_id: 1}, {$set: {a: 1}}));
        assert.writeOK(coll.remove({_id: 1}));
    }

    // Drop the second database, which should invalidate the stream.
    assert.commandWorked(testDB2.dropDatabase());

    // We should get 7 oplog entries; three ops of type insert, update, delete from each database,
    // and then an invalidate. The cursor should be closed.
    for (let expectedDB of[testDB1, testDB2]) {
        let change = cst.getOneChange(aggCursor);
        assert.eq(change.operationType, "insert", tojson(change));
        assert.eq(change.ns.db, expectedDB.getName(), tojson(change));
        change = cst.getOneChange(aggCursor);
        assert.eq(change.operationType, "update", tojson(change));
        assert.eq(change.ns.db, expectedDB.getName(), tojson(change));
        change = cst.getOneChange(aggCursor);
        assert.eq(change.operationType, "delete", tojson(change));
        assert.eq(change.ns.db, expectedDB.getName(), tojson(change));
    }
    cst.assertNextChangesEqual({
        cursor: aggCursor,
        expectedChanges: [{operationType: "invalidate"}],
        expectInvalidate: true
    });

    // Test that a cluster-wide change stream can be resumed using a token from a collection which
    // has been dropped.
    db1Coll = assertDropAndRecreateCollection(testDB1, jsTestName());

    // Get a valid resume token that the next change stream can use.
    aggCursor = cst.startWatchingAllChangesForCluster();

    assert.writeOK(db1Coll.insert({_id: 1}, {writeConcern: {w: "majority"}}));

    let change = cst.getOneChange(aggCursor, false);
    const resumeToken = change._id;

    // For cluster-wide streams, it is possible to resume at a point before a collection is dropped,
    // even if the invalidation has not been received on the original stream yet.
    assertDropCollection(db1Coll, db1Coll.getName());
    // Wait for two-phase drop to complete, so that the UUID no longer exists.
    assert.soon(function() {
        return !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(testDB1,
                                                                             db1Coll.getName());
    });
    assert.commandWorked(adminDB.runCommand({
        aggregate: 1,
        pipeline: [{$changeStream: {resumeAfter: resumeToken, allChangesForCluster: true}}],
        cursor: {}
    }));

    // Test that invalidation entries from any database invalidate the stream.
    [db1Coll, db2Coll] =
        [testDB1, testDB2].map((testDB) => assertDropAndRecreateCollection(testDB, jsTestName()));
    let _idForTest = 0;
    for (let collToInvalidate of[db1Coll, db2Coll]) {
        // Start watching all changes in the cluster.
        aggCursor = cst.startWatchingAllChangesForCluster();

        let testDB = collToInvalidate.getDB();

        // Insert into the collections on both databases, and verify the change stream is able to
        // pick them up.
        for (let collToWrite of[db1Coll, db2Coll]) {
            assert.writeOK(collToWrite.insert({_id: _idForTest}));
            change = cst.getOneChange(aggCursor);
            assert.eq(change.operationType, "insert", tojson(change));
            assert.eq(change.documentKey._id, _idForTest);
            assert.eq(change.ns.db, collToWrite.getDB().getName());
            _idForTest++;
        }

        // Renaming the collection should invalidate the change stream. Skip this test when running
        // on a sharded collection, since these cannot be renamed.
        if (!FixtureHelpers.isSharded(collToInvalidate)) {
            assert.writeOK(collToInvalidate.renameCollection("renamed_coll"));
            cst.assertNextChangesEqual({
                cursor: aggCursor,
                expectedChanges: [{operationType: "invalidate"}],
                expectInvalidate: true
            });
            collToInvalidate = testDB.getCollection("renamed_coll");
        }

        // Dropping a collection should invalidate the change stream.
        aggCursor = cst.startWatchingAllChangesForCluster();
        assertDropCollection(testDB, collToInvalidate.getName());
        cst.assertNextChangesEqual({
            cursor: aggCursor,
            expectedChanges: [{operationType: "invalidate"}],
            expectInvalidate: true
        });

        // Dropping a 'system' collection should invalidate the change stream.
        // Create a view to ensure that the 'system.views' collection exists.
        assert.commandWorked(
            testDB.runCommand({create: "view1", viewOn: collToInvalidate.getName(), pipeline: []}));
        aggCursor = cst.startWatchingAllChangesForCluster();
        assertDropCollection(testDB, "system.views");
        cst.assertNextChangesEqual({
            cursor: aggCursor,
            expectedChanges: [{operationType: "invalidate"}],
            expectInvalidate: true
        });
    }

    cst.cleanUp();
}());
