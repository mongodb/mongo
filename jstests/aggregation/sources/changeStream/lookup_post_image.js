// Tests the 'fullDocument' argument to the $changeStream stage.
//
// The $changeStream stage is not allowed within a $facet stage.
// @tags: [do_not_wrap_aggregations_in_facets]
(function() {
    "use strict";

    const replTest = new ReplSetTest({name: "changePostImage", nodes: 1});
    const nodes = replTest.startSet();
    replTest.initiate();
    replTest.awaitReplication();

    db = replTest.getPrimary().getDB("test");
    const coll = db.change_post_image;

    /**
     * Returns the last result from the first batch of the aggregation pipeline 'pipeline'.
     */
    function getLastResultFromFirstBatch({collection, pipeline}) {
        const cmdResponse = assert.commandWorked(
            db.runCommand({aggregate: collection.getName(), pipeline: pipeline, cursor: {}}));
        assert.neq(cmdResponse.cursor.firstBatch.length, 0);
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
        replTest.awaitReplication();
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "disableAwaitDataForGetMoreCmd", mode: "alwaysOn"}));
        let res = assert.commandWorked(db.runCommand({
            getMore: cursor.id,
            collection: getCollectionNameFromFullNamespace(cursor.ns),
            batchSize: 1
        }));
        assert.eq(res.cursor.nextBatch.length, 1);
        assert.commandWorked(
            db.adminCommand({configureFailPoint: "disableAwaitDataForGetMoreCmd", mode: "off"}));
        return res.cursor.nextBatch[0];
    }

    // Dummy document to give a resumeAfter point.
    db.createCollection(coll.getName());
    let res = assert.commandWorked(
        db.runCommand({aggregate: coll.getName(), pipeline: [{$changeStream: {}}], cursor: {}}));
    assert.writeOK(coll.insert({_id: "dummy"}));
    const firstChange = getOneDoc(res.cursor);

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

    // Test that looking up the post image of an update after the collection has been dropped will
    // result in 'fullDocument' with a value of null.
    coll.drop();
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

    // Test that invalidate entries don't have 'fullDocument' even if 'updateLookup' is specified.
    latestChange = getLastResultFromFirstBatch({
        collection: coll,
        pipeline: [
            {$changeStream: {fullDocument: "updateLookup", resumeAfter: firstChange._id}},
        ]
    });
    assert.eq(latestChange.operationType, "invalidate");
    assert(!latestChange.hasOwnProperty("fullDocument"));

    // TODO(russotto): Can just use "coll" here once read majority is working.
    // For now, using the old collection results in us reading stale data sometimes.
    const coll2 = db.real_get_more;
    jsTestLog("Testing full document lookup with a real getMore");
    assert.commandWorked(db.createCollection(coll2.getName()));
    assert.writeOK(coll2.insert({_id: "getMoreEnabled"}));
    replTest.awaitReplication();
    res = assert.commandWorked(db.runCommand({
        aggregate: coll2.getName(),
        pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
        cursor: {}
    }));
    assert.writeOK(coll2.update({_id: "getMoreEnabled"}, {$set: {updated: true}}));
    res = assert.commandWorked(db.runCommand({
        getMore: res.cursor.id,
        collection: getCollectionNameFromFullNamespace(res.cursor.ns),
        batchSize: 2
    }));
    assert.eq(res.cursor.nextBatch.length, 1);
    assert.docEq(res.cursor.nextBatch[0]["fullDocument"], {_id: "getMoreEnabled", updated: true});

    replTest.stopSet();
}());
