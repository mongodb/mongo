/*
 * Test that the read operations are not killed and their connections are also not
 * closed during step down.
 */
load('jstests/replsets/rslib.js');
load('jstests/libs/parallelTester.js');
load("jstests/libs/curop_helpers.js");  // for waitForCurOpByFailPoint().

(function() {

    "use strict";

    const testName = "readOpsDuringStepDown";
    const dbName = "test";
    const collName = "coll";

    var rst = new ReplSetTest({
        name: testName,
        nodes: [{setParameter: {closeConnectionsOnStepdown: false}}, {rsConfig: {priority: 0}}]
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);
    const primaryAdmin = primary.getDB("admin");
    const primaryColl = primaryDB[collName];
    const collNss = primaryColl.getFullName();

    TestData.dbName = dbName;
    TestData.collName = collName;

    jsTestLog("1. Do a document write");
    assert.writeOK(
        primaryColl.insert({_id: 0}, {"writeConcern": {"w": "majority"}}));
    rst.awaitReplication();

    // Open a cursor on primary.
    const cursorIdToBeReadAfterStepDown =
        assert.commandWorked(primaryDB.runCommand({"find": collName, batchSize: 0})).cursor.id;

    jsTestLog("2. Start blocking getMore cmd before step down");
    const joinGetMoreThread = startParallelShell(() => {
        // Open another cursor on primary before step down.
        primaryDB = db.getSiblingDB(TestData.dbName);
        const cursorIdToBeReadDuringStepDown =
            assert.commandWorked(primaryDB.runCommand({"find": TestData.collName, batchSize: 0}))
                .cursor.id;

        // Enable the fail point for get more cmd.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "waitAfterPinningCursorBeforeGetMoreBatch", mode: "alwaysOn"}));

        getMoreRes = assert.commandWorked(primaryDB.runCommand(
            {"getMore": cursorIdToBeReadDuringStepDown, collection: TestData.collName}));
        assert.docEq([{_id: 0}], getMoreRes.cursor.nextBatch);
    }, primary.port);

    // Wait for getmore cmd to reach the fail point.
    waitForCurOpByFailPoint(primaryAdmin, collNss, "waitAfterPinningCursorBeforeGetMoreBatch");

    jsTestLog("2. Start blocking find cmd before step down");
    const joinFindThread = startParallelShell(() => {
        // Enable the fail point for find cmd.
        assert.commandWorked(
            db.adminCommand({configureFailPoint: "waitInFindBeforeMakingBatch", mode: "alwaysOn"}));

        var findRes = assert.commandWorked(
            db.getSiblingDB(TestData.dbName).runCommand({"find": TestData.collName}));
        assert.docEq([{_id: 0}], findRes.cursor.firstBatch);

    }, primary.port);

    // Wait for find cmd to reach the fail point.
    waitForCurOpByFailPoint(primaryAdmin, collNss, "waitInFindBeforeMakingBatch");

    jsTestLog("3. Make primary step down");
    const joinStepDownThread = startParallelShell(() => {
        assert.commandWorked(db.adminCommand({"replSetStepDown": 100, "force": true}));
    }, primary.port);

    // Wait untill the step down has started to kill user operations.
    checkLog.contains(primary, "Starting to kill user operations");

    jsTestLog("4. Disable fail points");
    assert.commandWorked(
        primaryAdmin.runCommand({configureFailPoint: "waitInFindBeforeMakingBatch", mode: "off"}));
    assert.commandWorked(primaryAdmin.runCommand(
        {configureFailPoint: "waitAfterPinningCursorBeforeGetMoreBatch", mode: "off"}));

    // Wait for threads to join.
    joinGetMoreThread();
    joinFindThread();
    joinStepDownThread();

    // Wait untill the old primary transitioned to SECONDARY state.
    waitForState(primary, ReplSetTest.State.SECONDARY);

    jsTestLog("5. Start get more cmd after step down");
    var getMoreRes = assert.commandWorked(
        primaryDB.runCommand({"getMore": cursorIdToBeReadAfterStepDown, collection: collName}));
    assert.docEq([{_id: 0}], getMoreRes.cursor.nextBatch);

    rst.stopSet();
})();
