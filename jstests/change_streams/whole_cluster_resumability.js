// Basic tests for resuming a $changeStream that is open against all databases in a cluster.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.

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

    // Dropping a database should generate a 'drop' notification for the collection followed by a
    // 'dropDatabase' notification.
    assert.commandWorked(testDBs[0].dropDatabase());
    const dropDbChanges = cst.assertNextChangesEqual({
        cursor: resumeCursor,
        expectedChanges: [
            {operationType: "drop", ns: {db: testDBs[0].getName(), coll: db1Coll.getName()}},
            {operationType: "dropDatabase", ns: {db: testDBs[0].getName()}}
        ]
    });

    // Recreate the database and verify that the change stream picks up another insert.
    assert.writeOK(db1Coll.insert({_id: 5}));

    let change = cst.getOneChange(resumeCursor);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.ns, {db: testDBs[0].getName(), coll: db1Coll.getName()}, tojson(change));
    assert.eq(change.fullDocument, {_id: 5}, tojson(change));

    // Resume the change stream from the 'drop' entry.
    resumeCursor = cst.startWatchingChanges({
        pipeline:
            [{$changeStream: {resumeAfter: dropDbChanges[0]._id, allChangesForCluster: true}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    change = cst.getOneChange(resumeCursor);
    assert.eq(change.operationType, "dropDatabase", tojson(change));
    assert.eq(change.ns, {db: testDBs[0].getName()}, tojson(change));

    // Resume the change stream from the 'dropDatabase' entry.
    resumeCursor = cst.startWatchingChanges({
        pipeline:
            [{$changeStream: {resumeAfter: dropDbChanges[1]._id, allChangesForCluster: true}}],
        collection: 1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    change = cst.getOneChange(resumeCursor);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.ns, {db: testDBs[0].getName(), coll: db1Coll.getName()}, tojson(change));
    assert.eq(change.fullDocument, {_id: 5}, tojson(change));

    cst.cleanUp();
})();
