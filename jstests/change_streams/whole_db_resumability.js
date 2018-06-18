// Basic tests for resuming a $changeStream that is open against all collections in a database.
// Do not run in whole-cluster passthrough since this test assumes that the change stream will be
// invalidated by a database drop.
// @tags: [do_not_run_in_whole_cluster_passthrough]
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.

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

    // Explicitly drop one collection to ensure reliability of the order of notifications from the
    // dropDatabase command.
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
    const resumeTokenDrop = dropDbChanges[0]._id;
    const resumeTokenDropDb = dropDbChanges[1]._id;
    const resumeTokenInvalidate = dropDbChanges[2]._id;

    // Resume from the first collection drop.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: firstCollDrop._id}}],
        collection: 1,
    });
    cst.assertNextChangesEqual(
        {cursor: resumeCursor, expectedChanges: expectedChangesAfterFirstDrop});

    // Resume from the second collection drop.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: resumeTokenDrop}}],
        collection: 1,
    });
    cst.assertNextChangesEqual(
        {cursor: resumeCursor, expectedChanges: expectedChangesAfterFirstDrop.slice(1)});

    // Recreate the test collection.
    assert.writeOK(coll.insert({_id: "after recreate"}));

    let expectedInsert = [{
        operationType: "insert",
        ns: {db: testDB.getName(), coll: coll.getName()},
        fullDocument: {_id: "after recreate"},
        documentKey: {_id: "after recreate"}
    }];

    // Test resuming from the 'dropDatabase' entry.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: resumeTokenDropDb}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    cst.assertNextChangesEqual(
        {cursor: resumeCursor, expectedChanges: [{operationType: "invalidate"}]});

    // Test resuming from the 'invalidate' entry using 'resumeAfter'.
    assert.commandFailedWithCode(db.runCommand({
        aggregate: 1,
        pipeline: [{$changeStream: {resumeAfter: resumeTokenInvalidate}}],
        cursor: {},
        collation: {locale: "simple"},
    }),
                                 ErrorCodes.InvalidResumeToken);

    cst.cleanUp();
})();
