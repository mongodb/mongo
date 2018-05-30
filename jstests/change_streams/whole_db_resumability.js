// Basic tests for resuming a $changeStream that is open against all collections in a database.
// Do not run in whole-cluster passthrough since this test assumes that the change stream will be
// invalidated by a database drop.
// @tags: [do_not_run_in_whole_cluster_passthrough]
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.
    load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.

    // Drop and recreate the collections to be used in this set of tests.
    const testDB = db.getSiblingDB(jsTestName());
    let coll = assertDropAndRecreateCollection(testDB, "resume_coll");
    const otherColl = assertDropAndRecreateCollection(testDB, "resume_coll_other");

    let cst = new ChangeStreamTest(testDB);
    let resumeCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    // Insert a single document to each collection and save the resume token from the first insert.
    assert.writeOK(coll.insert({_id: 1}));
    assert.writeOK(otherColl.insert({_id: 2}));
    const firstInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(firstInsertChangeDoc.fullDocument, {_id: 1});
    assert.eq(firstInsertChangeDoc.ns, {db: testDB.getName(), coll: coll.getName()});

    // Test resuming the change stream after the first insert should pick up the insert on the
    // second collection.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: firstInsertChangeDoc._id}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });

    const secondInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(secondInsertChangeDoc.fullDocument, {_id: 2});
    assert.eq(secondInsertChangeDoc.ns, {db: testDB.getName(), coll: otherColl.getName()});

    // Insert a third document to the first collection and test that the change stream picks it up.
    assert.writeOK(coll.insert({_id: 3}));
    const thirdInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(thirdInsertChangeDoc.fullDocument, {_id: 3});
    assert.eq(thirdInsertChangeDoc.ns, {db: testDB.getName(), coll: coll.getName()});

    // Test resuming after the first insert again.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: firstInsertChangeDoc._id}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    assert.docEq(cst.getOneChange(resumeCursor), secondInsertChangeDoc);
    assert.docEq(cst.getOneChange(resumeCursor), thirdInsertChangeDoc);

    // Test resume after second insert.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: secondInsertChangeDoc._id}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    assert.docEq(cst.getOneChange(resumeCursor), thirdInsertChangeDoc);

    // Rename the collection and attempt to resume from the 'rename' notification. Skip this
    // test when running on a sharded collection, since these cannot be renamed.
    if (!FixtureHelpers.isSharded(coll)) {
        assertDropAndRecreateCollection(coll.getDB(), coll.getName());
        const renameColl = coll.getDB().getCollection("rename_coll");
        assertDropCollection(renameColl.getDB(), renameColl.getName());

        resumeCursor = cst.startWatchingChanges({collection: 1, pipeline: [{$changeStream: {}}]});
        assert.writeOK(coll.renameCollection(renameColl.getName()));

        const renameChanges = cst.assertNextChangesEqual({
            cursor: resumeCursor,
            expectedChanges: [
                {
                  operationType: "rename",
                  ns: {db: coll.getDB().getName(), coll: coll.getName()},
                  to: {db: renameColl.getDB().getName(), coll: renameColl.getName()}
                },
            ]
        });
        const resumeTokenRename = renameChanges[0]._id;

        // Insert into the renamed collection.
        assert.writeOK(renameColl.insert({_id: "after rename"}));

        // Resume from the rename notification using 'resumeAfter' and verify that the change stream
        // returns the next insert.
        let expectedInsert = {
            operationType: "insert",
            ns: {db: renameColl.getDB().getName(), coll: renameColl.getName()},
            fullDocument: {_id: "after rename"},
            documentKey: {_id: "after rename"}
        };
        resumeCursor = cst.startWatchingChanges(
            {collection: 1, pipeline: [{$changeStream: {resumeAfter: resumeTokenRename}}]});
        cst.assertNextChangesEqual({cursor: resumeCursor, expectedChanges: expectedInsert});

        // Resume from the rename notification using 'startAfter' and verify that the change stream
        // returns the next insert.
        expectedInsert = {
            operationType: "insert",
            ns: {db: renameColl.getDB().getName(), coll: renameColl.getName()},
            fullDocument: {_id: "after rename"},
            documentKey: {_id: "after rename"}
        };
        resumeCursor = cst.startWatchingChanges(
            {collection: 1, pipeline: [{$changeStream: {startAfter: resumeTokenRename}}]});
        cst.assertNextChangesEqual({cursor: resumeCursor, expectedChanges: expectedInsert});

        // Rename back to the original collection for reliability of the collection drops when
        // dropping the database.
        assert.writeOK(renameColl.renameCollection(coll.getName()));
    }

    // Explicitly drop one collection to ensure reliability of the order of notifications from the
    // dropDatabase command.
    resumeCursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});
    assertDropCollection(testDB, otherColl.getName());
    const firstCollDrop = cst.getOneChange(resumeCursor);
    assert.eq(firstCollDrop.operationType, "drop", tojson(firstCollDrop));
    assert.eq(firstCollDrop.ns, {db: testDB.getName(), coll: otherColl.getName()});

    // Dropping a database should generate a 'drop' notification for each collection, a
    // 'dropDatabase' notification, and finally an 'invalidate'.
    assert.commandWorked(testDB.dropDatabase());
    const expectedChangesAfterFirstDrop = [
        {operationType: "drop", ns: {db: testDB.getName(), coll: coll.getName()}},
        {operationType: "dropDatabase", ns: {db: testDB.getName()}},
        {operationType: "invalidate"}
    ];
    const dropDbChanges = cst.assertNextChangesEqual(
        {cursor: resumeCursor, expectedChanges: expectedChangesAfterFirstDrop});

    // Resume from the first collection drop.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: firstCollDrop._id}}],
        collection: 1,
    });
    cst.assertNextChangesEqual(
        {cursor: resumeCursor, expectedChanges: expectedChangesAfterFirstDrop});

    // Resume from the second collection drop using 'resumeAfter'.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: dropDbChanges[0]._id}}],
        collection: 1,
    });
    cst.assertNextChangesEqual(
        {cursor: resumeCursor, expectedChanges: expectedChangesAfterFirstDrop.slice(1)});

    // Resume from the second collection drop using 'startAfter'.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {startAfter: dropDbChanges[0]._id}}],
        collection: 1,
    });
    cst.assertNextChangesEqual(
        {cursor: resumeCursor, expectedChanges: expectedChangesAfterFirstDrop.slice(1)});

    // Recreate the test collection.
    coll = assertCreateCollection(testDB, coll.getName());
    assert.writeOK(coll.insert({_id: "after recreate"}));

    let expectedInsert = {
        operationType: "insert",
        ns: {db: db.getName(), coll: coll.getName()},
        fullDocument: {_id: "after recreate"},
        documentKey: {_id: "after recreate"}
    };

    // TODO SERVER-34789: The code below should throw an error. We exercise this behavior here to
    // be sure that it doesn't crash the server, but the ability to resume a change stream using
    // 'resumeAfter' with a resume token from an invalidate is a bug, not a feature.

    // Test resuming from the 'dropDatabase' entry using 'resumeAfter'.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: dropDbChanges[1]._id}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    cst.assertNextChangesEqual({cursor: resumeCursor, expectedChanges: expectedInsert});

    // Test resuming from the 'invalidate' entry using 'resumeAfter'.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: dropDbChanges[2]._id}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    cst.assertNextChangesEqual({cursor: resumeCursor, expectedChanges: expectedInsert});

    // Test resuming from the 'dropDatabase' entry using 'startAfter'.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {startAfter: dropDbChanges[1]._id}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    cst.assertNextChangesEqual({cursor: resumeCursor, expectedChanges: expectedInsert});

    // Test resuming from the 'invalidate' entry using 'startAfter' and verifies it picks up the
    // insert after recreating the db/collection.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: dropDbChanges[2]._id}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    cst.assertNextChangesEqual({cursor: resumeCursor, expectedChanges: expectedInsert});

    cst.cleanUp();
})();
