// Tests that an aggregate with a $changeStream stage will report the latest optime read in
// the oplog by its cursor. This is information is needed in order to correctly merge the results
// from the various shards on mongos.
(function() {
    "use strict";

    const testName = "report_latest_observed_oplog_timestamp";
    const cursorCollection = db.getCollection(testName);
    const otherCollection = db.getCollection("unrelated_" + testName);

    // Drop collections to assure a clean run.  Collections may not exist so do not check result.
    cursorCollection.drop();
    otherCollection.drop();

    // Get a resume point.
    jsTestLog("Getting a resume point.");
    const batchSize = 2;
    assert.commandWorked(db.createCollection(cursorCollection.getName()));
    const firstResponse = assert.commandWorked(cursorCollection.runCommand(
        {aggregate: testName, pipeline: [{$changeStream: {}}], cursor: {batchSize: batchSize}}));
    assert.eq(0, firstResponse.cursor.firstBatch.length);
    assert.writeOK(cursorCollection.insert({_id: 0}));

    function iterateCursor(initialCursorResponse) {
        const getMoreCollName = initialCursorResponse.cursor.ns.substr(
            initialCursorResponse.cursor.ns.indexOf('.') + 1);
        return assert.commandWorked(cursorCollection.runCommand({
            getMore: initialCursorResponse.cursor.id,
            collection: getMoreCollName,
            batchSize: batchSize
        }));
    }
    const resumeResponse = iterateCursor(firstResponse);
    assert.eq(1, resumeResponse.cursor.nextBatch.length);
    // Because needsMerge was not set, the latest oplog timestamp should not be returned.
    assert.eq(undefined, resumeResponse.$_internalLatestOplogTimestamp);
    const resumeToken = resumeResponse.cursor.nextBatch[0]["_id"];

    // Seed the collection with enough documents to fit in one batch.
    // Note the resume document is included; when needsMerge is true, we see the resume token
    // in the stream.
    jsTestLog("Adding documents to collection.");
    for (let i = 1; i < batchSize * 2; i++) {
        assert.writeOK(cursorCollection.insert({_id: i}, {writeConcern: {w: 1}}));
    }

    // TODO: SERVER-29126
    // While change streams still uses read concern level local instead of read concern level
    // majority, we need to use causal consistency to be able to immediately read our own writes out
    // of the oplog.  Once change streams read from the majority snapshot, we can remove this
    // synchronization point from this test.
    assert.commandWorked(db.runCommand({
        find: "foo",
        readConcern: {level: "local", afterClusterTime: db.getSession().getOperationTime()}
    }));

    // Look at one batch's worth.
    jsTestLog("Testing that operation time is present on initial aggregate command.");
    const cursorResponse = assert.commandWorked(cursorCollection.runCommand({
        aggregate: testName,
        // The latest observed optime is only reported when needsMerge is set, and needsMerge
        // requires fromMongos be set.
        needsMerge: true,
        fromMongos: true,
        pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
        cursor: {batchSize: batchSize}
    }));
    const firstBatchOplogTimestamp = cursorResponse.$_internalLatestOplogTimestamp;
    assert.neq(undefined, firstBatchOplogTimestamp, tojson(cursorResponse));

    // Iterate the cursor and assert that the observed operation time advanced.
    jsTestLog("Testing that operation time advances with getMore.");
    let getMoreResponse = iterateCursor(cursorResponse);
    const getMoreOplogTimestamp = getMoreResponse.$_internalLatestOplogTimestamp;
    assert.neq(undefined, getMoreOplogTimestamp, tojson(getMoreResponse));
    assert.gt(getMoreOplogTimestamp, firstBatchOplogTimestamp);

    // Now make sure that the reported operation time advances if there are writes to an unrelated
    // collection.
    jsTestLog("Testing that operation time advances with writes to an unrelated collection.");

    // First make sure there is nothing left in our cursor.
    getMoreResponse = iterateCursor(cursorResponse);
    assert.eq(getMoreResponse.cursor.nextBatch, []);

    // Record that operation time, then test that the reported time advances on an insert to an
    // unrelated collection.
    const oplogTimeAtExhaust = getMoreResponse.$_internalLatestOplogTimestamp;
    assert.neq(undefined, oplogTimeAtExhaust, tojson(getMoreResponse));
    assert.writeOK(otherCollection.insert({}));

    getMoreResponse = iterateCursor(cursorResponse);
    const oplogTimeAfterUnrelatedInsert = getMoreResponse.$_internalLatestOplogTimestamp;
    assert.neq(undefined, oplogTimeAtExhaust, tojson(getMoreResponse));
    assert.gt(oplogTimeAfterUnrelatedInsert, oplogTimeAtExhaust);
})();
