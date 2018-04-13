// Tests of metadata notifications for a $changeStream on a whole cluster.
(function() {
    "use strict";

    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.
    load('jstests/replsets/libs/two_phase_drops.js');  // For 'TwoPhaseDropCollectionTest'.
    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.

    // Define two databases. We will conduct our tests by creating one collection in each.
    const testDB1 = db.getSiblingDB(jsTestName()),
          testDB2 = db.getSiblingDB(jsTestName() + "_other");
    const adminDB = db.getSiblingDB("admin");

    assert.commandWorked(testDB1.dropDatabase());
    assert.commandWorked(testDB2.dropDatabase());

    // Create one collection on each database.
    let [db1Coll, db2Coll] =
        [testDB1, testDB2].map((testDB) => assertDropAndRecreateCollection(testDB, "test"));

    // Create a ChangeStreamTest on the 'admin' db. Cluster-wide change streams can only be opened
    // on admin.
    let cst = new ChangeStreamTest(adminDB);
    let aggCursor = cst.startWatchingAllChangesForCluster();

    // Generate oplog entries of type insert, update, and delete across both databases.
    for (let coll of[db1Coll, db2Coll]) {
        assert.writeOK(coll.insert({_id: 1}));
        assert.writeOK(coll.update({_id: 1}, {$set: {a: 1}}));
        assert.writeOK(coll.remove({_id: 1}));
    }

    // Drop the second database, which should invalidate the stream.
    assert.commandWorked(testDB2.dropDatabase());

    // We should get 6 oplog entries; three ops of type insert, update, delete from each database.
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
        expectedChanges: [
            {operationType: "drop", ns: {db: testDB2.getName(), coll: db2Coll.getName()}},
            // TODO SERVER-35029: Return an entry for a database drop, instead of "invalidate".
            {operationType: "invalidate"}
        ],
        expectInvalidate: true
    });

    // Test that a cluster-wide change stream can be resumed using a token from a collection which
    // has been dropped.
    db1Coll = assertDropAndRecreateCollection(testDB1, db1Coll.getName());

    // Get a valid resume token that the next change stream can use.
    aggCursor = cst.startWatchingAllChangesForCluster();

    assert.writeOK(db1Coll.insert({_id: 1}, {writeConcern: {w: "majority"}}));

    let change = cst.getOneChange(aggCursor, false);
    const resumeToken = change._id;

    // For cluster-wide streams, it is possible to resume at a point before a collection is dropped,
    // even if the "drop" notification has not been received on the original stream yet.
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

    // Test that collection drops from any database result in "drop" notifications for the stream.
    [db1Coll, db2Coll] =
        [testDB1, testDB2].map((testDB) => assertDropAndRecreateCollection(testDB, "test"));
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

        // Renaming the collection should generate a 'rename' notification. Skip this test when
        // running on a sharded collection, since these cannot be renamed.
        if (!FixtureHelpers.isSharded(collToInvalidate)) {
            assertDropAndRecreateCollection(testDB, collToInvalidate.getName());
            const collName = collToInvalidate.getName();

            // Start watching all changes in the cluster.
            aggCursor = cst.startWatchingAllChangesForCluster();
            assert.writeOK(collToInvalidate.renameCollection("renamed_coll"));
            cst.assertNextChangesEqual({
                cursor: aggCursor,
                expectedChanges: [
                    {
                      operationType: "rename",
                      ns: {db: testDB.getName(), coll: collToInvalidate.getName()},
                      to: {db: testDB.getName(), coll: "renamed_coll"}
                    },
                ]
            });

            // Repeat the test, this time using the 'dropTarget' option with an existing target
            // collection.
            collToInvalidate = testDB.getCollection("renamed_coll");
            assertDropAndRecreateCollection(testDB, collName);
            assert.writeOK(testDB[collName].insert({_id: 0}));
            assert.writeOK(collToInvalidate.renameCollection(collName, true /* dropTarget */));
            cst.assertNextChangesEqual({
                cursor: aggCursor,
                expectedChanges: [
                    {
                      operationType: "insert",
                      ns: {db: testDB.getName(), coll: collName},
                      documentKey: {_id: 0},
                      fullDocument: {_id: 0}
                    },
                    {
                      operationType: "rename",
                      ns: {db: testDB.getName(), coll: "renamed_coll"},
                      to: {db: testDB.getName(), coll: collName}
                    }
                ]
            });

            collToInvalidate = testDB[collName];

            // Test renaming a collection to a different database. Do not run this in the mongos
            // passthrough suites since we cannot guarantee the primary shard of the target database
            // and renameCollection requires the source and destination to be on the same shard.
            if (!FixtureHelpers.isMongos(testDB)) {
                const otherDB = testDB.getSiblingDB(testDB.getName() + "_rename_target");
                // Ensure the target database exists.
                const collOtherDB = assertDropAndRecreateCollection(otherDB, "test");
                assertDropCollection(otherDB, collOtherDB.getName());
                aggCursor = cst.startWatchingAllChangesForCluster();
                assert.commandWorked(testDB.adminCommand({
                    renameCollection: collToInvalidate.getFullName(),
                    to: collOtherDB.getFullName()
                }));
                // Do not check the 'ns' field since it will contain the namespace of the temp
                // collection created when renaming a collection across databases.
                change = cst.getOneChange(aggCursor);
                assert.eq(change.operationType, "rename", tojson(change));
                assert.eq(change.to,
                          {db: otherDB.getName(), coll: collOtherDB.getName()},
                          tojson(change));
                // Rename across databases also drops the source collection after the collection is
                // copied over.
                cst.assertNextChangesEqual({
                    cursor: aggCursor,
                    expectedChanges: [{
                        operationType: "drop",
                        ns: {db: testDB.getName(), coll: collToInvalidate.getName()}
                    }]
                });
            }

            // Test the behavior of a change stream watching the target collection of a $out
            // aggregation stage.
            collToInvalidate.aggregate([{$out: "renamed_coll"}]);
            // Do not check the 'ns' field since it will contain the namespace of the temp
            // collection created by the $out stage, before renaming to 'renamed_coll'.
            const rename = cst.getOneChange(aggCursor);
            assert.eq(rename.operationType, "rename", tojson(rename));
            assert.eq(rename.to, {db: testDB.getName(), coll: "renamed_coll"}, tojson(rename));

            // The change stream should not be invalidated by the rename(s).
            assert.eq(0, cst.getNextBatch(aggCursor).nextBatch.length);
            assert.writeOK(collToInvalidate.insert({_id: 2}));
            assert.eq(cst.getOneChange(aggCursor).operationType, "insert");
        }

        // Dropping a collection should generate a 'drop' entry.
        assertDropCollection(testDB, collToInvalidate.getName());
        cst.assertNextChangesEqual({
            cursor: aggCursor,
            expectedChanges: [{
                operationType: "drop",
                ns: {db: testDB.getName(), coll: collToInvalidate.getName()}
            }]
        });

        // Dropping a 'system' collection should also generate a 'drop' notification.
        // Create a view to ensure that the 'system.views' collection exists.
        assert.commandWorked(
            testDB.runCommand({create: "view1", viewOn: collToInvalidate.getName(), pipeline: []}));
        // TODO SERVER-35401: This insert is inconsistent with the behavior for whole-db change
        // streams.
        change = cst.getOneChange(aggCursor);
        assert.eq(change.operationType, "insert", tojson(change));
        assert.eq(change.ns, {db: testDB.getName(), coll: "system.views"});
        assertDropCollection(testDB, "system.views");
        cst.assertNextChangesEqual({
            cursor: aggCursor,
            expectedChanges:
                [{operationType: "drop", ns: {db: testDB.getName(), coll: "system.views"}}]
        });
    }

    cst.cleanUp();
}());
