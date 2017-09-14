// Tests the 'fullDocument' argument to the $changeStream stage.
//
// The $changeStream stage is not allowed within a $facet stage.
// @tags: [do_not_wrap_aggregations_in_facets]
(function() {
    "use strict";

    load("jstests/libs/fixture_helpers.js");           // For 'FixtureHelpers'.
    load("jstests/replsets/libs/two_phase_drops.js");  // For 'TwoPhaseDropCollectionTest'.

    const coll = db.change_post_image;

    /**
     * Returns the last result from the first batch of the aggregation pipeline 'pipeline'.
     */
    function getLastResultFromFirstBatch({collection, pipeline}) {
        // TODO: SERVER-29126
        // While change streams still uses read concern level local instead of read concern level
        // majority, we need to use causal consistency to be able to immediately read our own writes
        // out of the oplog.  Once change streams read from the majority snapshot, we can remove
        // these synchronization points from this test.
        assert.commandWorked(db.runCommand({
            find: "foo",
            readConcern: {level: "local", afterClusterTime: db.getMongo().getOperationTime()}
        }));

        const cmdResponse = assert.commandWorked(
            db.runCommand({aggregate: collection.getName(), pipeline: pipeline, cursor: {}}));
        assert.neq(cmdResponse.cursor.firstBatch.length, 0);
        assert.commandWorked(
            db.runCommand({killCursors: collection.getName(), cursors: [cmdResponse.cursor.id]}));
        return cmdResponse.cursor.firstBatch[cmdResponse.cursor.firstBatch.length - 1];
    }

    function getCollectionNameFromFullNamespace(ns) {
        return ns.split(/\.(.+)/)[1];
    }

    /**
     * Gets one document from the cursor using getMore with awaitData disabled. Asserts if no
     * document is present.
     */
    function getOneDoc(cursor) {
        // TODO: SERVER-29126
        assert.commandWorked(db.runCommand({
            find: "foo",
            readConcern: {level: "local", afterClusterTime: db.getMongo().getOperationTime()}
        }));
        FixtureHelpers.awaitReplication();
        // TODO: SERVER-29126
        // While change streams still uses read concern level local instead of read concern level
        // majority, we need to use causal consistency to be able to immediately read our own writes
        // out of the oplog.  Once change streams read from the majority snapshot, we can remove
        // these synchronization points from this test.
        assert.commandWorked(db.runCommand({
            find: "foo",
            readConcern: {level: "local", afterClusterTime: db.getMongo().getOperationTime()}
        }));
        FixtureHelpers.runCommandOnEachPrimary({
            dbName: "admin",
            cmdObj: {configureFailPoint: "disableAwaitDataForGetMoreCmd", mode: "alwaysOn"}
        });
        let res = assert.commandWorked(db.runCommand({
            getMore: cursor.id,
            collection: getCollectionNameFromFullNamespace(cursor.ns),
            batchSize: 1
        }));
        assert.eq(res.cursor.nextBatch.length, 1);
        FixtureHelpers.runCommandOnEachPrimary({
            dbName: "admin",
            cmdObj: {configureFailPoint: "disableAwaitDataForGetMoreCmd", mode: "off"}
        });
        return res.cursor.nextBatch[0];
    }

    // Dummy document to give a resumeAfter point.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName()));
    let res = assert.commandWorked(
        db.runCommand({aggregate: coll.getName(), pipeline: [{$changeStream: {}}], cursor: {}}));
    assert.writeOK(coll.insert({_id: "dummy"}));
    const firstChange = getOneDoc(res.cursor);
    assert.commandWorked(db.runCommand({killCursors: coll.getName(), cursors: [res.cursor.id]}));

    jsTestLog("Testing change streams without 'fullDocument' specified");
    // Test that not specifying 'fullDocument' does include a 'fullDocument' in the result for an
    // insert.
    assert.writeOK(coll.insert({_id: "fullDocument not specified"}));
    let latestChange = getLastResultFromFirstBatch(
        {collection: coll, pipeline: [{$changeStream: {resumeAfter: firstChange._id}}]});
    assert.eq(latestChange.operationType, "insert");
    assert.eq(latestChange.fullDocument, {_id: "fullDocument not specified"});

    // Test that not specifying 'fullDocument' does include a 'fullDocument' in the result for a
    // replacement-style update.
    assert.writeOK(coll.update({_id: "fullDocument not specified"},
                               {_id: "fullDocument not specified", replaced: true}));
    latestChange = getLastResultFromFirstBatch(
        {collection: coll, pipeline: [{$changeStream: {resumeAfter: firstChange._id}}]});
    assert.eq(latestChange.operationType, "replace");
    assert.eq(latestChange.fullDocument, {_id: "fullDocument not specified", replaced: true});

    // Test that not specifying 'fullDocument' does not include a 'fullDocument' in the result for
    // a non-replacement update.
    assert.writeOK(coll.update({_id: "fullDocument not specified"}, {$set: {updated: true}}));
    latestChange = getLastResultFromFirstBatch(
        {collection: coll, pipeline: [{$changeStream: {resumeAfter: firstChange._id}}]});
    assert.eq(latestChange.operationType, "update");
    assert(!latestChange.hasOwnProperty("fullDocument"));

    jsTestLog("Testing change streams with 'fullDocument' specified as 'default'");

    // Test that specifying 'fullDocument' as 'default' does include a 'fullDocument' in the result
    // for an insert.
    assert.writeOK(coll.insert({_id: "fullDocument is default"}));
    latestChange = getLastResultFromFirstBatch({
        collection: coll,
        pipeline: [{$changeStream: {fullDocument: "default", resumeAfter: firstChange._id}}]
    });
    assert.eq(latestChange.operationType, "insert");
    assert.eq(latestChange.fullDocument, {_id: "fullDocument is default"});

    // Test that specifying 'fullDocument' as 'default' does include a 'fullDocument' in the result
    // for a replacement-style update.
    assert.writeOK(coll.update({_id: "fullDocument is default"},
                               {_id: "fullDocument is default", replaced: true}));
    latestChange = getLastResultFromFirstBatch(
        {collection: coll, pipeline: [{$changeStream: {resumeAfter: firstChange._id}}]});
    assert.eq(latestChange.operationType, "replace");
    assert.eq(latestChange.fullDocument, {_id: "fullDocument is default", replaced: true});

    // Test that specifying 'fullDocument' as 'default' does not include a 'fullDocument' in the
    // result for a non-replacement update.
    assert.writeOK(coll.update({_id: "fullDocument is default"}, {$set: {updated: true}}));
    latestChange = getLastResultFromFirstBatch(
        {collection: coll, pipeline: [{$changeStream: {resumeAfter: firstChange._id}}]});
    assert.eq(latestChange.operationType, "update");
    assert(!latestChange.hasOwnProperty("fullDocument"));

    jsTestLog("Testing change streams with 'fullDocument' specified as 'updateLookup'");

    // Test that specifying 'fullDocument' as 'updateLookup' does include a 'fullDocument' in the
    // result for an insert.
    assert.writeOK(coll.insert({_id: "fullDocument is lookup"}));
    latestChange = getLastResultFromFirstBatch({
        collection: coll,
        pipeline: [{$changeStream: {fullDocument: "updateLookup", resumeAfter: firstChange._id}}]
    });
    assert.eq(latestChange.operationType, "insert");
    assert.eq(latestChange.fullDocument, {_id: "fullDocument is lookup"});

    // Test that specifying 'fullDocument' as 'updateLookup' does include a 'fullDocument' in the
    // result for a replacement-style update.
    assert.writeOK(coll.update({_id: "fullDocument is lookup"},
                               {_id: "fullDocument is lookup", replaced: true}));
    latestChange = getLastResultFromFirstBatch({
        collection: coll,
        pipeline: [{$changeStream: {fullDocument: "updateLookup", resumeAfter: firstChange._id}}]
    });
    assert.eq(latestChange.operationType, "replace");
    assert.eq(latestChange.fullDocument, {_id: "fullDocument is lookup", replaced: true});

    // Test that specifying 'fullDocument' as 'updateLookup' does include a 'fullDocument' in the
    // result for a non-replacement update.
    assert.writeOK(coll.update({_id: "fullDocument is lookup"}, {$set: {updated: true}}));
    latestChange = getLastResultFromFirstBatch({
        collection: coll,
        pipeline: [{$changeStream: {fullDocument: "updateLookup", resumeAfter: firstChange._id}}]
    });
    assert.eq(latestChange.operationType, "update");
    assert.eq(latestChange.fullDocument,
              {_id: "fullDocument is lookup", replaced: true, updated: true});

    // Test that looking up the post image of an update after deleting the document will result in a
    // 'fullDocument' with a value of null.
    assert.writeOK(coll.remove({_id: "fullDocument is lookup"}));
    latestChange = getLastResultFromFirstBatch({
        collection: coll,
        pipeline: [
            {$changeStream: {fullDocument: "updateLookup", resumeAfter: firstChange._id}},
            {$match: {operationType: "update"}}
        ]
    });
    assert.eq(latestChange.operationType, "update");
    assert(latestChange.hasOwnProperty("fullDocument"));
    assert.eq(latestChange.fullDocument, null);
    const deleteDocResumePoint = latestChange._id;

    // Test that looking up the post image of an update after the collection has been dropped will
    // result in 'fullDocument' with a value of null.  This must be done using getMore because new
    // cursors cannot be established after a collection drop.
    assert.writeOK(coll.insert({_id: "fullDocument is lookup 2"}));
    assert.writeOK(coll.update({_id: "fullDocument is lookup 2"}, {$set: {updated: true}}));
    res = assert.commandWorked(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [
            {$changeStream: {fullDocument: "updateLookup", resumeAfter: deleteDocResumePoint}},
            {$match: {operationType: "update"}}
        ],
        cursor: {batchSize: 0}
    }));
    assert.neq(res.cursor.id, 0);
    // Save another stream to test lookup after the collecton gets recreated.
    const resBeforeDrop = assert.commandWorked(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [
            {$changeStream: {fullDocument: "updateLookup", resumeAfter: deleteDocResumePoint}},
            {$match: {operationType: "update"}}
        ],
        cursor: {batchSize: 0}
    }));
    assert.neq(resBeforeDrop.cursor.id, 0);

    coll.drop();
    // Wait until two-phase drop finishes.
    assert.soon(function() {
        return !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(db, coll.getName());
    });

    latestChange = getOneDoc(res.cursor);
    assert.eq(latestChange.operationType, "update");
    assert(latestChange.hasOwnProperty("fullDocument"));
    assert.eq(latestChange.fullDocument, null);

    // Test establishing new cursors with resume token on dropped collections failes.
    res = db.runCommand({
        aggregate: coll.getName(),
        pipeline: [
            {$changeStream: {fullDocument: "updateLookup", resumeAfter: deleteDocResumePoint}},
            {$match: {operationType: "update"}}
        ],
        cursor: {batchSize: 0}
    });
    assert.commandFailedWithCode(res, 40615);

    // Test that looking up the post image of an update after the collection has been dropped and
    // created again will result in 'fullDocument' with a value of null. This must be done using
    // getMore because new cursors cannot be established after a collection drop.
    //
    // Insert a document with the same _id, verify the change stream won't return it due to
    // different UUID.
    assert.commandWorked(db.createCollection(coll.getName()));
    assert.writeOK(coll.insert({_id: "fullDocument is lookup 2"}));
    latestChange = getOneDoc(resBeforeDrop.cursor);
    assert.eq(latestChange.operationType, "update");
    assert(latestChange.hasOwnProperty("fullDocument"));
    assert.eq(latestChange.fullDocument, null);

    // Test that invalidate entries don't have 'fullDocument' even if 'updateLookup' is specified.
    db.collInvalidate.drop();
    assert.commandWorked(db.createCollection(db.collInvalidate.getName()));
    res = assert.commandWorked(db.runCommand({
        aggregate: db.collInvalidate.getName(),
        pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
        cursor: {batchSize: 0}
    }));
    assert.writeOK(db.collInvalidate.insert({_id: "testing invalidate"}));
    assert.neq(res.cursor.id, 0);
    db.collInvalidate.drop();
    // Wait until two-phase drop finishes.
    assert.soon(function() {
        return !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(
            db, db.collInvalidate.getName());
    });
    latestChange = getOneDoc(res.cursor);
    assert.eq(latestChange.operationType, "insert");
    latestChange = getOneDoc(res.cursor);
    assert.eq(latestChange.operationType, "invalidate");
    assert(!latestChange.hasOwnProperty("fullDocument"));
    assert.commandWorked(
        db.runCommand({killCursors: db.collInvalidate.getName(), cursors: [res.cursor.id]}));

    // TODO(russotto): Can just use "coll" here once read majority is working.
    // For now, using the old collection results in us reading stale data sometimes.
    jsTestLog("Testing full document lookup with a real getMore");
    const coll2 = db.real_get_more;
    coll2.drop();
    assert.commandWorked(db.createCollection(coll2.getName()));
    assert.writeOK(coll2.insert({_id: "getMoreEnabled"}));
    FixtureHelpers.awaitReplication();
    // TODO: SERVER-29126
    // While change streams still uses read concern level local instead of read concern level
    // majority, we need to use causal consistency to be able to immediately read our own writes
    // out of the oplog.  Once change streams read from the majority snapshot, we can remove
    // these synchronization points from this test.
    assert.commandWorked(db.runCommand({
        find: "foo",
        readConcern: {level: "local", afterClusterTime: db.getMongo().getOperationTime()}
    }));

    res = assert.commandWorked(db.runCommand({
        aggregate: coll2.getName(),
        pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
        cursor: {}
    }));
    assert.writeOK(coll2.update({_id: "getMoreEnabled"}, {$set: {updated: true}}));

    // TODO: SERVER-29126
    // While change streams still uses read concern level local instead of read concern level
    // majority, we need to use causal consistency to be able to immediately read our own writes
    // out of the oplog.  Once change streams read from the majority snapshot, we can remove
    // these synchronization points from this test.
    assert.commandWorked(db.runCommand({
        find: "foo",
        readConcern: {level: "local", afterClusterTime: db.getMongo().getOperationTime()}
    }));

    res = assert.commandWorked(db.runCommand({
        getMore: res.cursor.id,
        collection: getCollectionNameFromFullNamespace(res.cursor.ns),
        batchSize: 2
    }));
    assert.eq(res.cursor.nextBatch.length, 1);
    assert.docEq(res.cursor.nextBatch[0]["fullDocument"], {_id: "getMoreEnabled", updated: true});
    assert.commandWorked(db.runCommand({killCursors: coll2.getName(), cursors: [res.cursor.id]}));
}());
