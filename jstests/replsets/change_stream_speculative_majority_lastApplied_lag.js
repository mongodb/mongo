/**
 * Test speculative majority change stream reads against a primary when the replication system's
 * 'lastApplied' optime lags behind the timestamp of the newest oplog entry visible in the storage
 * layer. Ensure that we do not return uncommitted data in this case.
 *
 * @tags: [uses_speculative_majority]
 */
(function() {
    "use strict";

    load('jstests/libs/change_stream_util.js');  // For ChangeStreamTest.
    load("jstests/libs/check_log.js");           // For checkLog.
    load("jstests/libs/parallelTester.js");      // for ScopedThread.

    const name = "change_stream_speculative_majority_lastApplied_lag";
    const replTest = new ReplSetTest({
        name: name,
        nodes: [{}, {rsConfig: {priority: 0}}],
        nodeOptions: {enableMajorityReadConcern: 'false'}
    });
    replTest.startSet();
    replTest.initiate();

    const dbName = name;
    const collName = "coll";

    const primary = replTest.getPrimary();
    const primaryDB = primary.getDB(dbName);
    const primaryColl = primaryDB[collName];

    // Do a few operations on the primary and let them both majority commit. Later on we will
    // receive both of these operations in a change stream.
    let res = assert.commandWorked(primaryColl.runCommand(
        "insert", {documents: [{_id: 1, v: 0}], writeConcern: {w: "majority"}}));
    assert.commandWorked(
        primaryColl.update({_id: 1}, {$set: {v: 1}}, {writeConcern: {w: "majority"}}));

    // Save this operation time so we can start a change stream from here.
    let startOperTime = res.operationTime;

    // Make the primary hang after it has completed a write but before it has advanced lastApplied
    // for that write.
    primaryDB.adminCommand(
        {configureFailPoint: "hangBeforeLogOpAdvancesLastApplied", mode: "alwaysOn"});

    // Function which will be used by the background thread to perform an update on the specified
    // host, database, and collection.
    function doUpdate(host, dbName, collName, query, update) {
        let hostDB = (new Mongo(host)).getDB(dbName);
        assert.commandWorked(hostDB[collName].update(query, update));
    }

    // Do a document update on primary, but don't wait for it to majority commit. The write should
    // hang due to the enabled failpoint.
    jsTestLog("Starting update on primary.");
    var primaryWrite =
        new ScopedThread(doUpdate, primary.host, dbName, collName, {_id: 1}, {$set: {v: 2}});
    primaryWrite.start();

    // Wait for the fail point to be hit. By the time the primary hits this fail point, the update
    // should be visible. 'lastApplied', however, has not yet been advanced yet. We check both the
    // document state and the logs to make sure we hit the failpoint for the correct operation.
    assert.soon(() => (primaryColl.findOne({_id: 1}).v === 2));
    checkLog.contains(primary, 'hangBeforeLogOpAdvancesLastApplied fail point enabled.');

    // Open a change stream on the primary. The stream should only return the initial insert and the
    // first of the two update events, since the second update is not yet majority-committed.
    // Despite the fact that the effects of the latter update are already visible to local readers,
    // speculative majority will read at min(lastApplied, allCommitted), and so change stream's
    // 'fullDocument' lookup should also *not* return the second update's uncommitted changes.
    jsTestLog("Opening a change stream on the primary.");
    const cst = new ChangeStreamTest(primaryDB);
    let cursor = cst.startWatchingChanges({
        pipeline:
            [{$changeStream: {startAtOperationTime: startOperTime, fullDocument: "updateLookup"}}],
        collection: collName
    });

    cst.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: [
            {
              documentKey: {_id: 1},
              fullDocument: {_id: 1, v: 0},
              ns: {db: dbName, coll: collName},
              operationType: "insert",
            },
            {
              documentKey: {_id: 1},
              fullDocument: {_id: 1, v: 1},
              ns: {db: dbName, coll: collName},
              updateDescription: {removedFields: [], updatedFields: {v: 1}},
              operationType: "update",
            }
        ]
    });

    // Make sure the cursor does not return any more change events.
    cursor = cst.getNextBatch(cursor);
    assert.eq(cursor.nextBatch.length, 0);

    // Disable the failpoint to let the test complete.
    primaryDB.adminCommand({configureFailPoint: "hangBeforeLogOpAdvancesLastApplied", mode: "off"});

    primaryWrite.join();
    replTest.stopSet();
})();