// Tests that the $changeStream requires read concern majority.
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");           // For startSetIfSupportsReadMajority.
    load("jstests/libs/write_concern_util.js");  // For stopReplicationOnSecondaries.
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {enableMajorityReadConcern: ""}});
    if (!startSetIfSupportsReadMajority(rst)) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }
    rst.initiate();

    const oplogProjection = {$project: {"_id.clusterTime": 0}};
    const name = "change_stream_require_majority_read_concern";
    const db = rst.getPrimary().getDB(name);

    function getCollectionNameFromFullNamespace(ns) {
        return ns.split(/\.(.+)/)[1];
    }

    // Helpers for testing that pipeline returns correct set of results.  Run startWatchingChanges
    // with the pipeline, then insert the changes, then run assertNextBatchMatches with the result
    // of startWatchingChanges and the expected set of results.
    function startWatchingChanges({pipeline, collection, includeTs, aggregateOptions}) {
        aggregateOptions = aggregateOptions || {cursor: {}};

        if (!includeTs) {
            // Strip the oplog fields we aren't testing.
            pipeline.push(oplogProjection);
        }

        let res = assert.commandWorked(db.runCommand(
            Object.merge({aggregate: collection.getName(), pipeline: pipeline}, aggregateOptions)));
        assert.neq(res.cursor.id, 0);
        return res.cursor;
    }

    // Gets one document from the cursor using getMore with awaitData disabled. Asserts if no
    // document is present.
    function getOneDoc(cursor) {
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

    // Attempts to get a document from the cursor with awaitData disabled, and asserts if a
    // document is present.
    function assertNextBatchIsEmpty(cursor) {
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "disableAwaitDataForGetMoreCmd", mode: "alwaysOn"}));
        let res = assert.commandWorked(db.runCommand({
            getMore: cursor.id,
            collection: getCollectionNameFromFullNamespace(cursor.ns),
            batchSize: 1
        }));
        assert.eq(res.cursor.nextBatch.length, 0);
        assert.commandWorked(
            db.adminCommand({configureFailPoint: "disableAwaitDataForGetMoreCmd", mode: "off"}));
    }

    // Test read concerns other than "majority" are not supported.
    const primaryColl = db.foo;
    assert.writeOK(primaryColl.insert({_id: 1}, {writeConcern: {w: "majority"}}));
    let res = primaryColl.runCommand({
        aggregate: primaryColl.getName(),
        pipeline: [{$changeStream: {}}],
        cursor: {},
        readConcern: {level: "local"},
    });
    assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);
    res = primaryColl.runCommand({
        aggregate: primaryColl.getName(),
        pipeline: [{$changeStream: {}}],
        cursor: {},
        readConcern: {level: "linearizable"},
    });
    assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);

    // Test that explicit read concern "majority" works.
    res = primaryColl.runCommand({
        aggregate: primaryColl.getName(),
        pipeline: [{$changeStream: {}}],
        cursor: {},
        readConcern: {level: "majority"},
    });
    assert.commandWorked(res);

    // Test not specifying readConcern defaults to "majority" read concern.
    stopReplicationOnSecondaries(rst);
    // Verify that the document just inserted cannot be returned.
    let cursor = startWatchingChanges({pipeline: [{$changeStream: {}}], collection: primaryColl});
    assert.eq(cursor.firstBatch.length, 0);

    // Insert a document on the primary only.
    assert.writeOK(primaryColl.insert({_id: 2}, {writeConcern: {w: 1}}));
    assertNextBatchIsEmpty(cursor);

    // Restart data replicaiton and wait until the new write becomes visible.
    restartReplicationOnSecondaries(rst);
    rst.awaitLastOpCommitted();

    // Verify that the expected doc is returned because it has been committed.
    let doc = getOneDoc(cursor);
    assert.docEq(doc.operationType, "insert");
    assert.docEq(doc.fullDocument, {_id: 2});
}());
