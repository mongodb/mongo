/**
 * Test that a change stream on the primary node survives stepdown.
 */
(function() {
    "use strict";

    load("jstests/libs/write_concern_util.js");  // for [stop|restart]ServerReplication.

    const name = "change_stream_speculative_majority";
    const replTest = new ReplSetTest({
        name: name,
        nodes: [{setParameter: {closeConnectionsOnStepdown: false}}, {}],
        nodeOptions: {enableMajorityReadConcern: 'false'}
    });
    replTest.startSet();
    replTest.initiate();

    const dbName = name;
    const collName = "change_stream_stepdown";
    const changeStreamComment = collName + "_comment";

    const primary = replTest.getPrimary();
    const secondary = replTest.getSecondary();
    const primaryDb = primary.getDB(dbName);
    const secondaryDb = secondary.getDB(dbName);
    const primaryColl = primaryDb[collName];

    // Tell the secondary to stay secondary until we say otherwise.
    assert.commandWorked(secondaryDb.adminCommand({replSetFreeze: 999999}));

    // Open a change stream.
    let res = primaryDb.runCommand({
        aggregate: collName,
        pipeline: [{$changeStream: {}}],
        cursor: {},
        comment: changeStreamComment,
        maxTimeMS: 5000
    });
    assert.commandWorked(res);
    let cursorId = res.cursor.id;

    // Insert several documents on primary and let them majority commit.
    assert.commandWorked(
        primaryColl.insert([{_id: 1}, {_id: 2}, {_id: 3}], {writeConcern: {w: "majority"}}));
    replTest.awaitReplication();

    jsTestLog("Testing that changestream survives stepdown between find and getmore");
    // Step down.
    assert.commandWorked(primaryDb.adminCommand({replSetStepDown: 60, force: true}));
    replTest.waitForState(primary, ReplSetTest.State.SECONDARY);

    // Receive the first change event.  This tests stepdown between find and getmore.
    res = assert.commandWorked(
        primaryDb.runCommand({getMore: cursorId, collection: collName, batchSize: 1}));
    let changes = res.cursor.nextBatch;
    assert.eq(changes.length, 1);
    assert.eq(changes[0]["fullDocument"], {_id: 1});
    assert.eq(changes[0]["operationType"], "insert");

    jsTestLog("Testing that changestream survives step-up");
    // Step back up and wait for primary.
    assert.commandWorked(primaryDb.adminCommand({replSetFreeze: 0}));
    replTest.getPrimary();

    // Get the next one.  This tests that changestreams survives a step-up.
    res = assert.commandWorked(
        primaryDb.runCommand({getMore: cursorId, collection: collName, batchSize: 1}));
    changes = res.cursor.nextBatch;
    assert.eq(changes.length, 1);
    assert.eq(changes[0]["fullDocument"], {_id: 2});
    assert.eq(changes[0]["operationType"], "insert");

    jsTestLog("Testing that changestream survives stepdown between two getmores");
    // Step down again.
    assert.commandWorked(primaryDb.adminCommand({replSetStepDown: 60, force: true}));
    replTest.waitForState(primary, ReplSetTest.State.SECONDARY);

    // Get the next one.  This tests that changestreams survives a step down between getmores.
    res = assert.commandWorked(
        primaryDb.runCommand({getMore: cursorId, collection: collName, batchSize: 1}));
    changes = res.cursor.nextBatch;
    assert.eq(changes.length, 1);
    assert.eq(changes[0]["fullDocument"], {_id: 3});
    assert.eq(changes[0]["operationType"], "insert");

    // Step back up and wait for primary.
    assert.commandWorked(primaryDb.adminCommand({replSetFreeze: 0}));
    replTest.getPrimary();

    jsTestLog("Testing that changestream waiting on old primary sees docs inserted on new primary");

    replTest.awaitReplication();  // Ensure secondary is up to date and can win an election.
    TestData.changeStreamComment = changeStreamComment;
    TestData.secondaryHost = secondary.host;
    TestData.dbName = dbName;
    TestData.collName = collName;
    let waitForShell = startParallelShell(function() {
        // Wait for the getMore to be in progress.
        assert.soon(
            () => db.getSiblingDB("admin")
                      .aggregate([
                          {'$currentOp': {}},
                          {
                            '$match': {
                                op: 'getmore',
                                'cursor.originatingCommand.comment': TestData.changeStreamComment
                            }
                          }
                      ])
                      .itcount() == 1);

        const secondary = new Mongo(TestData.secondaryHost);
        const secondaryDb = secondary.getDB(TestData.dbName);
        // Step down the old primary and wait for new primary.
        assert.commandWorked(secondaryDb.adminCommand({replSetFreeze: 0}));
        assert.commandWorked(secondaryDb.adminCommand({replSetStepUp: 1, skipDryRun: true}));
        jsTestLog("Waiting for new primary");
        assert.soon(() => secondaryDb.adminCommand({isMaster: 1}).ismaster);

        jsTestLog("Inserting document on new primary");
        assert.commandWorked(secondaryDb[TestData.collName].insert({_id: 4}),
                             {writeConcern: {w: "majority"}});
    }, primary.port);

    res = assert.commandWorked(primaryDb.runCommand({
        getMore: cursorId,
        collection: collName,
        batchSize: 1,
        maxTimeMS: ReplSetTest.kDefaultTimeoutMS
    }));
    changes = res.cursor.nextBatch;
    assert.eq(changes.length, 1);
    assert.eq(changes[0]["fullDocument"], {_id: 4});
    assert.eq(changes[0]["operationType"], "insert");

    waitForShell();

    replTest.stopSet();
})();
