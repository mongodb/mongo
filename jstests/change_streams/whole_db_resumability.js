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

    // Resume from the first collection drop.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: firstCollDrop._id}}],
        collection: 1,
    });
    cst.assertNextChangesEqual(
        {cursor: resumeCursor, expectedChanges: expectedChangesAfterFirstDrop});

    // Resume from the second collection drop.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: dropDbChanges[0]._id}}],
        collection: 1,
    });
    cst.assertNextChangesEqual(
        {cursor: resumeCursor, expectedChanges: expectedChangesAfterFirstDrop.slice(1)});

    // Recreate the test collection.
    coll = assertCreateCollection(testDB, coll.getName());
    assert.writeOK(coll.insert({_id: 0}));

    // Test resuming from the 'dropDatabase' entry.
    // TODO SERVER-34789: Resuming from the 'dropDatabase' should return a single invalidate
    // notification.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: dropDbChanges[1]._id}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    let change = cst.getOneChange(resumeCursor);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.fullDocument, {_id: 0}, tojson(change));
    assert.eq(change.ns, {db: testDB.getName(), coll: coll.getName()}, tojson(change));

    // Test resuming from the 'invalidate' entry.
    // TODO SERVER-34789: Resuming from an invalidate should error or return an invalidate
    // notification.
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: dropDbChanges[2]._id}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    change = cst.getOneChange(resumeCursor);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.fullDocument, {_id: 0}, tojson(change));
    assert.eq(change.ns, {db: testDB.getName(), coll: coll.getName()}, tojson(change));

    cst.cleanUp();
})();
