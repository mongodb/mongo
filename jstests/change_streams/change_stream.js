// Basic $changeStream tests.
// Mark as assumes_read_preference_unchanged since reading from the non-replicated "system.profile"
// collection results in a failure in the secondary reads suite.
// @tags: [assumes_read_preference_unchanged]
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.
    load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest and
                                                       // assert[Valid|Invalid]ChangeStreamNss.

    const isMongos = FixtureHelpers.isMongos(db);

    // Drop and recreate the collections to be used in this set of tests.
    assertDropAndRecreateCollection(db, "t1");
    assertDropAndRecreateCollection(db, "t2");

    // Test that $changeStream only accepts an object as its argument.
    function checkArgFails(arg) {
        assert.commandFailedWithCode(
            db.runCommand({aggregate: "t1", pipeline: [{$changeStream: arg}], cursor: {}}), 50808);
    }

    checkArgFails(1);
    checkArgFails("invalid");
    checkArgFails(false);
    checkArgFails([1, 2, "invalid", {x: 1}]);

    // Test that a change stream cannot be opened on collections in the "admin", "config", or
    // "local" databases.
    assertInvalidChangeStreamNss("admin", "testColl");
    assertInvalidChangeStreamNss("config", "testColl");
    // Not allowed to access 'local' database through mongos.
    if (!isMongos) {
        assertInvalidChangeStreamNss("local", "testColl");
    }

    // Test that a change stream cannot be opened on 'system.' collections.
    assertInvalidChangeStreamNss(db.getName(), "system.users");
    assertInvalidChangeStreamNss(db.getName(), "system.profile");
    assertInvalidChangeStreamNss(db.getName(), "system.version");

    // Test that a change stream can be opened on namespaces with 'system' in the name, but not
    // considered an internal 'system dot' namespace.
    assertValidChangeStreamNss(db.getName(), "systemindexes");
    assertValidChangeStreamNss(db.getName(), "system_users");

    // Similar test but for DB names that are not considered internal.
    assert.writeOK(db.getSiblingDB("admincustomDB")["test"].insert({}));
    assertValidChangeStreamNss("admincustomDB");

    assert.writeOK(db.getSiblingDB("local_")["test"].insert({}));
    assertValidChangeStreamNss("local_");

    assert.writeOK(db.getSiblingDB("_config_")["test"].insert({}));
    assertValidChangeStreamNss("_config_");

    let cst = new ChangeStreamTest(db);
    let cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});

    jsTestLog("Testing single insert");
    // Test that if there are no changes, we return an empty batch.
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    assert.writeOK(db.t1.insert({_id: 0, a: 1}));
    let expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 1},
        ns: {db: "test", coll: "t1"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Test that if there are no changes during a subsequent 'getMore', we return an empty batch.
    cursor = cst.getNextBatch(cursor);
    assert.eq(0, cursor.nextBatch.length, "Cursor had changes: " + tojson(cursor));

    jsTestLog("Testing second insert");
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    assert.writeOK(db.t1.insert({_id: 1, a: 2}));
    expected = {
        documentKey: {_id: 1},
        fullDocument: {_id: 1, a: 2},
        ns: {db: "test", coll: "t1"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    jsTestLog("Testing update");
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    assert.writeOK(db.t1.update({_id: 0}, {_id: 0, a: 3}));
    expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 3},
        ns: {db: "test", coll: "t1"},
        operationType: "replace",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    jsTestLog("Testing update of another field");
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    assert.writeOK(db.t1.update({_id: 0}, {_id: 0, b: 3}));
    expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, b: 3},
        ns: {db: "test", coll: "t1"},
        operationType: "replace",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    jsTestLog("Testing upsert");
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    assert.writeOK(db.t1.update({_id: 2}, {_id: 2, a: 4}, {upsert: true}));
    expected = {
        documentKey: {_id: 2},
        fullDocument: {_id: 2, a: 4},
        ns: {db: "test", coll: "t1"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    jsTestLog("Testing partial update with $inc");
    assert.writeOK(db.t1.insert({_id: 3, a: 5, b: 1}));
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    assert.writeOK(db.t1.update({_id: 3}, {$inc: {b: 2}}));
    expected = {
        documentKey: {_id: 3},
        ns: {db: "test", coll: "t1"},
        operationType: "update",
        updateDescription: {removedFields: [], updatedFields: {b: 3}},
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    jsTestLog("Testing delete");
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    assert.writeOK(db.t1.remove({_id: 1}));
    expected = {
        documentKey: {_id: 1},
        ns: {db: "test", coll: "t1"},
        operationType: "delete",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    jsTestLog("Testing intervening write on another collection");
    cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t1});
    let t2cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.t2});
    assert.writeOK(db.t2.insert({_id: 100, c: 1}));
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: []});
    expected = {
        documentKey: {_id: 100},
        fullDocument: {_id: 100, c: 1},
        ns: {db: "test", coll: "t2"},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: t2cursor, expectedChanges: [expected]});

    jsTestLog("Testing drop of unrelated collection");
    assert.writeOK(db.dropping.insert({}));
    assertDropCollection(db, db.dropping.getName());
    // Should still see the previous change from t2, shouldn't see anything about 'dropping'.

    jsTestLog("Testing insert that looks like rename");
    assertDropCollection(db, "dne1");
    assertDropCollection(db, "dne2");
    const dne1cursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.dne1});
    const dne2cursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.dne2});
    assert.writeOK(db.t2.insert({_id: 101, renameCollection: "test.dne1", to: "test.dne2"}));
    cst.assertNextChangesEqual({cursor: dne1cursor, expectedChanges: []});
    cst.assertNextChangesEqual({cursor: dne2cursor, expectedChanges: []});

    if (!isMongos) {
        jsTestLog("Ensuring attempt to read with legacy operations fails.");
        db.getMongo().forceReadMode('legacy');
        const legacyCursor =
            db.tailable2.aggregate([{$changeStream: {}}], {cursor: {batchSize: 0}});
        assert.throws(function() {
            legacyCursor.next();
        }, [], "Legacy getMore expected to fail on changeStream cursor.");
        db.getMongo().forceReadMode('commands');
    }

    jsTestLog("Testing resumability");
    assertDropAndRecreateCollection(db, "resume1");

    // Note we do not project away 'id.ts' as it is part of the resume token.
    let resumeCursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: db.resume1});

    // Insert a document and save the resulting change stream.
    assert.writeOK(db.resume1.insert({_id: 1}));
    const firstInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(firstInsertChangeDoc.fullDocument, {_id: 1});

    jsTestLog("Testing resume after one document.");
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: firstInsertChangeDoc._id}}],
        collection: db.resume1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });

    jsTestLog("Inserting additional documents.");
    assert.writeOK(db.resume1.insert({_id: 2}));
    const secondInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(secondInsertChangeDoc.fullDocument, {_id: 2});
    assert.writeOK(db.resume1.insert({_id: 3}));
    const thirdInsertChangeDoc = cst.getOneChange(resumeCursor);
    assert.docEq(thirdInsertChangeDoc.fullDocument, {_id: 3});

    jsTestLog("Testing resume after first document of three.");
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: firstInsertChangeDoc._id}}],
        collection: db.resume1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    assert.docEq(cst.getOneChange(resumeCursor), secondInsertChangeDoc);
    assert.docEq(cst.getOneChange(resumeCursor), thirdInsertChangeDoc);

    jsTestLog("Testing resume after second document of three.");
    resumeCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: secondInsertChangeDoc._id}}],
        collection: db.resume1,
        aggregateOptions: {cursor: {batchSize: 0}},
    });
    assert.docEq(cst.getOneChange(resumeCursor), thirdInsertChangeDoc);

    cst.cleanUp();
}());
