/**
 * Test change stream behavior with speculative majority reads in the face of replication rollback.
 *
 * @tags: [uses_speculative_majority]
 */
(function() {
    'use strict';

    load("jstests/replsets/libs/rollback_test.js");  // for RollbackTest.

    // Disable implicit sessions so it's easy to run commands from different threads.
    TestData.disableImplicitSessions = true;

    const name = "change_stream_speculative_majority_rollback";
    const dbName = name;
    const collName = "coll";

    // Set up a replica set for use in RollbackTest. We disable majority reads on all nodes so we
    // will utilize speculative majority reads for change streams.
    const replTest = new ReplSetTest({
        name,
        nodes: 3,
        useBridge: true,
        settings: {chainingAllowed: false},
        nodeOptions: {enableMajorityReadConcern: "false"}
    });
    replTest.startSet();
    let config = replTest.getReplSetConfig();
    config.members[2].priority = 0;
    replTest.initiate(config);

    const rollbackTest = new RollbackTest(name, replTest);
    const primary = rollbackTest.getPrimary();
    const primaryDB = primary.getDB(dbName);
    let coll = primaryDB[collName];

    // Create a collection.
    assert.commandWorked(coll.insert({_id: 0}, {writeConcern: {w: "majority"}}));

    // Open a change stream on the initial primary.
    let res =
        primaryDB.runCommand({aggregate: collName, pipeline: [{$changeStream: {}}], cursor: {}});
    assert.commandWorked(res);
    let cursorId = res.cursor.id;

    // Receive an initial change event and save the resume token.
    assert.commandWorked(coll.insert({_id: 1}, {writeConcern: {w: "majority"}}));
    res = primaryDB.runCommand({getMore: cursorId, collection: collName});
    let changes = res.cursor.nextBatch;
    assert.eq(changes.length, 1);
    assert.eq(changes[0]["fullDocument"], {_id: 1});
    assert.eq(changes[0]["operationType"], "insert");
    let resumeToken = changes[0]["_id"];

    let rollbackNode = rollbackTest.transitionToRollbackOperations();
    assert.eq(rollbackNode, primary);

    // Insert a few items that will be rolled back.
    assert.commandWorked(coll.insert({_id: 2}));
    assert.commandWorked(coll.insert({_id: 3}));
    assert.commandWorked(coll.insert({_id: 4}));

    let getChangeEvent = new ScopedThread(function(host, cursorId, dbName, collName) {
        jsTestLog("Trying to receive change event from divergent primary.");
        const nodeDB = new Mongo(host).getDB(dbName);
        try {
            return nodeDB.runCommand({getMore: eval(cursorId), collection: collName});
        } catch (e) {
            return isNetworkError(e);
        }
    }, rollbackNode.host, tojson(cursorId), dbName, collName);
    getChangeEvent.start();

    // Make sure the change stream query started.
    assert.soon(() => primaryDB.currentOp({"command.getMore": cursorId}).inprog.length === 1);

    // Do some operations on the new primary that we can receive in a resumed stream.
    let syncSource = rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    coll = syncSource.getDB(dbName)[collName];
    assert.commandWorked(coll.insert({_id: 5}));
    assert.commandWorked(coll.insert({_id: 6}));
    assert.commandWorked(coll.insert({_id: 7}));

    // Let rollback begin and complete.
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    // The change stream query should have failed when the node entered rollback.
    assert(getChangeEvent.returnData());

    jsTestLog("Resuming change stream against new primary.");
    res = syncSource.getDB(dbName).runCommand(
        {aggregate: collName, pipeline: [{$changeStream: {resumeAfter: resumeToken}}], cursor: {}});
    changes = res.cursor.firstBatch;
    assert.eq(changes.length, 3);
    assert.eq(changes[0]["fullDocument"], {_id: 5});
    assert.eq(changes[0]["operationType"], "insert");
    assert.eq(changes[1]["fullDocument"], {_id: 6});
    assert.eq(changes[1]["operationType"], "insert");
    assert.eq(changes[2]["fullDocument"], {_id: 7});
    assert.eq(changes[2]["operationType"], "insert");

    rollbackTest.stop();

})();
