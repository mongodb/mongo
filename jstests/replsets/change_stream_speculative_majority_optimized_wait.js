/**
 * Verify that speculative majority change stream oplog reads only wait on the latest scanned oplog
 * optime, as opposed to the newest system-wide applied optime. This is an optimization to reduce
 * unnecessary waiting on the server.
 *
 * @tags: [uses_speculative_majority]
 */
(function() {
    "use strict";

    load("jstests/libs/write_concern_util.js");  // for [stop|restart]ServerReplication.

    const name = "change_stream_speculative_majority";
    const replTest = new ReplSetTest({
        name: name,
        nodes: [{}, {rsConfig: {priority: 0}}],
        nodeOptions: {enableMajorityReadConcern: 'false'}
    });
    replTest.startSet();
    replTest.initiate();

    const dbName = name;
    const collName = "coll";

    let primary = replTest.getPrimary();
    let secondary = replTest.getSecondary();
    let primaryDB = primary.getDB(dbName);
    let primaryColl = primaryDB[collName];

    // Receive 1 change to get an initial resume token.
    let res = assert.commandWorked(
        primaryDB.runCommand({aggregate: collName, pipeline: [{$changeStream: {}}], cursor: {}}));
    let cursorId = res.cursor.id;
    assert.commandWorked(primaryColl.insert({_id: 0}, {writeConcern: {w: "majority"}}));
    res = primary.getDB(dbName).runCommand({getMore: cursorId, collection: collName});
    assert.eq(res.cursor.nextBatch.length, 1);
    let resumeToken = res.cursor.nextBatch[0]["_id"];

    // Open a change stream.
    res = assert.commandWorked(
        primaryDB.runCommand({aggregate: collName, pipeline: [{$changeStream: {}}], cursor: {}}));
    cursorId = res.cursor.id;

    // Insert documents to fill one batch and let them majority commit.
    let batchSize = 2;
    assert.commandWorked(primaryColl.insert({_id: 1}, {writeConcern: {w: "majority"}}));
    assert.commandWorked(primaryColl.insert({_id: 2}, {writeConcern: {w: "majority"}}));

    // Pause replication on the secondary so that writes won't majority commit.
    stopServerReplication(secondary);

    // Do write on primary that won't majority commit but will advance the last applied optime.
    assert.commandWorked(primaryColl.insert({_id: 3}));

    // Receive one batch of change events. We should be able to read only the majority committed
    // change events and no further in order to generate this batch.
    res = assert.commandWorked(primary.getDB(dbName).runCommand(
        {getMore: cursorId, collection: collName, batchSize: batchSize}));
    let changes = res.cursor.nextBatch;
    assert.eq(changes.length, 2);
    assert.eq(changes[0]["fullDocument"], {_id: 1});
    assert.eq(changes[0]["operationType"], "insert");
    assert.eq(changes[1]["fullDocument"], {_id: 2});
    assert.eq(changes[1]["operationType"], "insert");

    // Make sure that 'aggregate' commands also utilize the optimization.
    res = assert.commandWorked(primaryDB.runCommand({
        aggregate: collName,
        pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
        cursor: {batchSize: batchSize}
    }));
    changes = res.cursor.firstBatch;
    assert.eq(changes.length, 2);
    assert.eq(changes[0]["fullDocument"], {_id: 1});
    assert.eq(changes[0]["operationType"], "insert");
    assert.eq(changes[1]["fullDocument"], {_id: 2});
    assert.eq(changes[1]["operationType"], "insert");

    // Let the test finish.
    restartServerReplication(secondary);

    replTest.stopSet();
})();