// Tests of $changeStream notifications for metadata operations.
// Do not run in whole-cluster passthrough since this test assumes that the change stream will be
// invalidated by a database drop.
// @tags: [do_not_run_in_whole_cluster_passthrough]
(function() {
    "use strict";

    load("jstests/libs/change_stream_util.js");
    load('jstests/replsets/libs/two_phase_drops.js');  // For 'TwoPhaseDropCollectionTest'.
    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    db = db.getSiblingDB(jsTestName());
    let cst = new ChangeStreamTest(db);

    db.getMongo().forceReadMode('commands');

    // Test that it is possible to open a new change stream cursor on a collection that does not
    // exist.
    const collName = "test";
    assertDropCollection(db, collName);

    // Cursor creation succeeds, but there are no results. We do not expect to see a notification
    // for collection creation.
    let cursor = cst.startWatchingChanges(
        {collection: collName, pipeline: [{$changeStream: {}}, {$project: {operationType: 1}}]});

    // We explicitly test getMore, to ensure that the getMore command for a non-existent collection
    // does not return an error.
    let change = cst.getNextBatch(cursor);
    assert.neq(change.id, 0);
    assert.eq(change.nextBatch.length, 0, tojson(change.nextBatch));

    // Dropping the empty database should not generate any notification for the change stream, since
    // the collection does not exist yet.
    assert.commandWorked(db.dropDatabase());
    change = cst.getNextBatch(cursor);
    assert.neq(change.id, 0);
    assert.eq(change.nextBatch.length, 0, tojson(change.nextBatch));

    // After collection creation, we expect to see oplog entries for each subsequent operation.
    let coll = assertCreateCollection(db, collName);
    assert.writeOK(coll.insert({_id: 0}));

    change = cst.getOneChange(cursor);
    assert.eq(change.operationType, "insert", tojson(change));

    // Create oplog entries of type insert, update, delete, and drop.
    assert.writeOK(coll.insert({_id: 1}));
    assert.writeOK(coll.update({_id: 1}, {$set: {a: 1}}));
    assert.writeOK(coll.remove({_id: 1}));
    assertDropCollection(db, coll.getName());

    // Get a valid resume token that the next aggregate command can use.
    change = cst.getOneChange(cursor);
    assert.eq(change.operationType, "insert");
    const resumeToken = change._id;

    // We should get 4 oplog entries of type update, delete, drop, and invalidate. The cursor should
    // be closed.
    let expectedChanges = [
        {operationType: "update"},
        {operationType: "delete"},
        {operationType: "drop"},
        {operationType: "invalidate"},
    ];
    const changes = cst.assertNextChangesEqual(
        {cursor: cursor, expectedChanges: expectedChanges, expectInvalidate: true});

    // It should not be possible to resume a change stream after a collection drop without an
    // explicit collation, even if the invalidate has not been received.
    assert.commandFailedWithCode(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
        cursor: {}
    }),
                                 ErrorCodes.InvalidResumeToken);

    // Recreate the collection.
    coll = assertCreateCollection(db, collName);
    assert.writeOK(coll.insert({_id: 0}));

    // TODO SERVER-34789: The code below should throw an error. We exercise this behavior here to
    // be sure that it doesn't crash the server, but the ability to resume a change stream after an
    // invalidate is a bug, not a feature.

    // Test resuming the change stream from the collection drop.
    assert.doesNotThrow(function() {
        const resumeTokenDrop = changes[2]._id;
        const resumeCursor =
            coll.watch([], {resumeAfter: resumeTokenDrop, collation: {locale: "simple"}});
        assert.soon(() => resumeCursor.hasNext());
        // Not checking the contents of the document returned, because we do not technically
        // support this behavior.
        resumeCursor.next();
    });
    // Test resuming the change stream from the invalidate after the drop.
    assert.doesNotThrow(function() {
        const resumeTokenInvalidate = changes[3]._id;
        const resumeCursor =
            coll.watch([], {resumeAfter: resumeTokenInvalidate, collation: {locale: "simple"}});
        assert.soon(() => resumeCursor.hasNext());
        // Not checking the contents of the document returned, because we do not technically
        // support this behavior.
        resumeCursor.next();
    });

    // Test that renaming a collection being watched generates a "rename" entry followed by an
    // "invalidate". This is true if the change stream is on the source or target collection of the
    // rename. Sharded collections cannot be renamed.
    if (!FixtureHelpers.isSharded(coll)) {
        cursor = cst.startWatchingChanges({collection: collName, pipeline: [{$changeStream: {}}]});
        assertDropCollection(db, "renamed_coll");
        assert.writeOK(coll.renameCollection("renamed_coll"));
        let expected = [
            {
              operationType: "rename",
              ns: {db: db.getName(), coll: collName},
              to: {db: db.getName(), coll: "renamed_coll"},
            },
            {operationType: "invalidate"}
        ];
        cst.assertNextChangesEqual(
            {cursor: cursor, expectedChanges: expected, expectInvalidate: true});

        coll = db["renamed_coll"];

        // Repeat the test, this time with a change stream open on the target.
        cursor = cst.startWatchingChanges({collection: collName, pipeline: [{$changeStream: {}}]});
        assert.writeOK(coll.renameCollection(collName));
        expected = [
            {
              operationType: "rename",
              ns: {db: db.getName(), coll: "renamed_coll"},
              to: {db: db.getName(), coll: collName},
            },
            {operationType: "invalidate"}
        ];
        const changes = cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});

        coll = db[collName];

        // TODO SERVER-34789: The code below should throw an error. We exercise this behavior here
        // to be sure that it doesn't crash the server, but the ability to resume a change stream
        // after an invalidate is a bug, not a feature.

        // Test resuming the change stream from the collection rename.
        assert.doesNotThrow(function() {
            const resumeTokenRename = changes[0]._id;
            const resumeCursor =
                coll.watch([], {resumeAfter: resumeTokenRename, collation: {locale: "simple"}});
        });
        // Test resuming the change stream from the invalidate after the drop.
        assert.doesNotThrow(function() {
            const resumeTokenInvalidate = changes[1]._id;
            const resumeCursor =
                coll.watch([], {resumeAfter: resumeTokenInvalidate, collation: {locale: "simple"}});
        });

        assertDropAndRecreateCollection(db, "renamed_coll");
        assert.writeOK(db.renamed_coll.insert({_id: 0}));

        // Repeat the test again, this time using the 'dropTarget' option with an existing target
        // collection.
        cursor =
            cst.startWatchingChanges({collection: "renamed_coll", pipeline: [{$changeStream: {}}]});
        assert.writeOK(coll.renameCollection("renamed_coll", true /* dropTarget */));
        expected = [
            {
              operationType: "rename",
              ns: {db: db.getName(), coll: collName},
              to: {db: db.getName(), coll: "renamed_coll"},
            },
            {operationType: "invalidate"}
        ];
        cst.assertNextChangesEqual(
            {cursor: cursor, expectedChanges: expected, expectInvalidate: true});

        coll = db["renamed_coll"];

        // Test the behavior of a change stream watching the target collection of a $out aggregation
        // stage.
        cursor = cst.startWatchingChanges({collection: collName, pipeline: [{$changeStream: {}}]});
        coll.aggregate([{$out: collName}]);
        // Note that $out will first create a temp collection, and then rename the temp collection
        // to the target. Do not explicitly check the 'ns' field.
        const rename = cst.getOneChange(cursor);
        assert.eq(rename.operationType, "rename", tojson(rename));
        assert.eq(rename.to, {db: db.getName(), coll: collName}, tojson(rename));
        assert.eq(cst.getOneChange(cursor, true).operationType, "invalidate");
    }

    // Test that dropping a database will first drop all of it's collections, invalidating any
    // change streams on those collections.
    cursor = cst.startWatchingChanges({
        collection: coll.getName(),
        pipeline: [{$changeStream: {}}],
    });
    assert.commandWorked(db.dropDatabase());

    expectedChanges = [
        {
          operationType: "drop",
          ns: {db: db.getName(), coll: coll.getName()},
        },
        {operationType: "invalidate"}
    ];
    cst.assertNextChangesEqual(
        {cursor: cursor, expectedChanges: expectedChanges, expectInvalidate: true});

    cst.cleanUp();
}());
