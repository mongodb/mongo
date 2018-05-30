// Basic tests for resuming a $changeStream that is open against all databases in a cluster.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.
    load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.

    // Create two databases, with one collection in each.
    const testDBs = [db.getSiblingDB(jsTestName()), db.getSiblingDB(jsTestName() + "_other")];
    let [db1Coll, db2Coll] = testDBs.map((db) => assertDropAndRecreateCollection(db, "test"));
    const adminDB = db.getSiblingDB("admin");

    let cst = new ChangeStreamTest(adminDB);
    let resumeCursor = cst.startWatchingAllChangesForCluster();

    // Insert a document in the first database and save the resulting change stream.
    assert.writeOK(db1Coll.insert({_id: 1}));
    const firstInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(firstInsertChangeDoc.fullDocument, {_id: 1});

    // Test resume after the first insert.
    resumeCursor = cst.startWatchingChanges({
        pipeline:
            [{$changeStream: {resumeAfter: firstInsertChangeDoc._id, allChangesForCluster: true}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });

    // Write the next document into the second database.
    assert.writeOK(db2Coll.insert({_id: 2}));
    const secondInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(secondInsertChangeDoc.fullDocument, {_id: 2});

    // Write the third document into the first database again.
    assert.writeOK(db1Coll.insert({_id: 3}));
    const thirdInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(thirdInsertChangeDoc.fullDocument, {_id: 3});

    // Test resuming after the first insert again.
    resumeCursor = cst.startWatchingChanges({
        pipeline:
            [{$changeStream: {resumeAfter: firstInsertChangeDoc._id, allChangesForCluster: true}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    assert.docEq(cst.getOneChange(resumeCursor), secondInsertChangeDoc);
    assert.docEq(cst.getOneChange(resumeCursor), thirdInsertChangeDoc);

    // Test resume after second insert.
    resumeCursor = cst.startWatchingChanges({
        pipeline:
            [{$changeStream: {resumeAfter: secondInsertChangeDoc._id, allChangesForCluster: true}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    assert.docEq(cst.getOneChange(resumeCursor), thirdInsertChangeDoc);

    // Rename the collection and obtain a resume token from the 'rename' notification. Skip this
    // test when running on a sharded collection, since these cannot be renamed.
    if (!FixtureHelpers.isSharded(db1Coll)) {
        assertDropAndRecreateCollection(db1Coll.getDB(), db1Coll.getName());
        const renameColl = db1Coll.getDB().getCollection("rename_coll");
        assertDropCollection(renameColl.getDB(), renameColl.getName());

        resumeCursor = cst.startWatchingChanges({
            collection: 1,
            pipeline: [{$changeStream: {allChangesForCluster: true}}],
            aggregateOptions: {cursor: {batchSize: 0}}
        });
        assert.writeOK(db1Coll.renameCollection(renameColl.getName()));

        const renameChanges = cst.assertNextChangesEqual({
            cursor: resumeCursor,
            expectedChanges: [
                {
                  operationType: "rename",
                  ns: {db: db1Coll.getDB().getName(), coll: db1Coll.getName()},
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
        resumeCursor = cst.startWatchingChanges({
            collection: 1,
            pipeline:
                [{$changeStream: {resumeAfter: resumeTokenRename, allChangesForCluster: true}}],
            aggregateOptions: {cursor: {batchSize: 0}}
        });
        cst.assertNextChangesEqual({cursor: resumeCursor, expectedChanges: expectedInsert});

        // Resume from the rename notification using 'startAfter' and verify that the change stream
        // returns the next insert.
        expectedInsert = {
            operationType: "insert",
            ns: {db: renameColl.getDB().getName(), coll: renameColl.getName()},
            fullDocument: {_id: "after rename"},
            documentKey: {_id: "after rename"}
        };
        resumeCursor = cst.startWatchingChanges({
            collection: 1,
            pipeline:
                [{$changeStream: {startAfter: resumeTokenRename, allChangesForCluster: true}}],
            aggregateOptions: {cursor: {batchSize: 0}}
        });
        cst.assertNextChangesEqual({cursor: resumeCursor, expectedChanges: expectedInsert});

        // Rename back to the original collection for reliability of the collection drops when
        // dropping the database.
        assert.writeOK(renameColl.renameCollection(db1Coll.getName()));
    }

    // Dropping a database should generate a 'drop' notification for the collection followed by a
    // 'dropDatabase' notification.
    resumeCursor = cst.startWatchingAllChangesForCluster();
    assert.commandWorked(testDBs[0].dropDatabase());
    const dropDbChanges = cst.assertNextChangesEqual({
        cursor: resumeCursor,
        expectedChanges: [
            {operationType: "drop", ns: {db: testDBs[0].getName(), coll: db1Coll.getName()}},
            {operationType: "dropDatabase", ns: {db: testDBs[0].getName()}}
        ]
    });
    const resumeTokenDbDrop = dropDbChanges[1]._id;

    // Recreate the collection and insert a document.
    assert.writeOK(db1Coll.insert({_id: "after recreate"}));

    let expectedInsert = {
        operationType: "insert",
        ns: {db: testDBs[0].getName(), coll: db1Coll.getName()},
        fullDocument: {_id: "after recreate"},
        documentKey: {_id: "after recreate"}
    };

    // Resume from the database drop using 'resumeAfter', and verify the change stream picks up
    // the insert.
    resumeCursor = cst.startWatchingChanges({
        collection: 1,
        pipeline: [{$changeStream: {resumeAfter: resumeTokenDbDrop, allChangesForCluster: true}}]
    });
    cst.assertNextChangesEqual({cursor: resumeCursor, expectedChanges: expectedInsert});

    // Resume from the database drop using 'startAfter', and verify the change stream picks up the
    // insert.
    resumeCursor = cst.startWatchingChanges({
        collection: 1,
        pipeline: [{$changeStream: {startAfter: resumeTokenDbDrop, allChangesForCluster: true}}]
    });
    cst.assertNextChangesEqual({cursor: resumeCursor, expectedChanges: expectedInsert});

    cst.cleanUp();
})();
