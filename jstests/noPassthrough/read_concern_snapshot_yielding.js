// Test that the read concern level 'snapshot' exhibits the correct yielding behavior for find and
// getMore. That is, snapshot reads check for interrupt but do not yield locks.
(function() {
    "use strict";

    load("jstests/libs/profiler.js");  // For getLatestProfilerEntry.

    const dbName = "test";
    const collName = "coll";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const db = rst.getPrimary().getDB(dbName);
    const adminDB = db.getSiblingDB("admin");
    const coll = db.coll;

    TestData.sessionId = assert.commandWorked(adminDB.runCommand({startSession: 1})).id;
    TestData.txnNumber = 0;

    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));
    db.setProfilingLevel(2);

    if (!db.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        rst.stopSet();
        return;
    }

    function waitForOpId(curOpFilter) {
        let opId;
        assert.soon(
            function() {
                const res = adminDB
                                .aggregate([
                                    {$currentOp: {}},
                                    {$match: {$and: [{ns: coll.getFullName()}, curOpFilter]}}
                                ])
                                .toArray();
                if (res.length === 1) {
                    opId = res[0].opid;
                    return true;
                }
                return false;
            },
            function() {
                return "Failed to find operation in $currentOp output: " +
                    tojson(adminDB.aggregate([{$currentOp: {}}, {$match: {ns: coll.getFullName()}}])
                               .toArray());
            });
        return opId;
    }

    function assertKillPending(opId) {
        const res =
            adminDB.aggregate([{$currentOp: {}}, {$match: {ns: coll.getFullName(), opid: opId}}])
                .toArray();
        assert.eq(res.length,
                  1,
                  tojson(adminDB.aggregate([{$currentOp: {}}, {$match: {ns: coll.getFullName()}}])
                             .toArray()));
        assert(res[0].hasOwnProperty("killPending"), tojson(res));
        assert.eq(true, res[0].killPending, tojson(res));
    }

    function awaitFindFn() {
        assert.commandWorked(db.runCommand({
            find: "coll",
            filter: {x: 1},
            readConcern: {level: "snapshot"},
            lsid: TestData.sessionId,
            txnNumber: NumberLong(TestData.txnNumber)
        }));
    }

    function awaitGetMoreFn() {
        const cursorId = assert
                             .commandWorked(db.runCommand({
                                 find: "coll",
                                 filter: {x: 1},
                                 batchSize: 2,
                                 readConcern: {level: "snapshot"},
                                 lsid: TestData.sessionId,
                                 txnNumber: NumberLong(TestData.txnNumber)
                             }))
                             .cursor.id;
        assert.commandWorked(
            db.adminCommand({configureFailPoint: "setCheckForInterruptHang", mode: "alwaysOn"}));
        assert.commandWorked(db.runCommand({
            getMore: NumberLong(cursorId),
            collection: "coll",
            lsid: TestData.sessionId,
            txnNumber: NumberLong(TestData.txnNumber)
        }));
    }

    for (let i = 0; i < 4; i++) {
        assert.commandWorked(db.coll.insert({_id: i, x: 1}, {writeConcern: {w: "majority"}}));
    }

    // Perform an initial find prior to setting the 'setCheckForInterruptHang' failpoint. As the
    // first read on a session, this will setup the session. Included in setup is a read on the
    // config.transactions collection. If the session is setup while the failpoint is active
    // we will block on the config.transaction read and not the user operation we are testing.
    assert.commandWorked(db.runCommand({
        find: "coll",
        filter: {x: 1},
        lsid: TestData.sessionId,
        txnNumber: NumberLong(TestData.txnNumber)
    }));

    //
    // Snapshot finds can be killed.
    //
    TestData.txnNumber++;

    // Start a find command that hangs before checking for interrupt.
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "setCheckForInterruptHang", mode: "alwaysOn"}));
    let awaitFind = startParallelShell(awaitFindFn, rst.ports[0]);

    // Kill the command, and check that it is set to killPending.
    let opId = waitForOpId({"command.filter": {x: 1}});
    assert.commandWorked(db.killOp(opId));
    assertKillPending(opId);

    // Remove the hang, and check that the command is killed.
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "setCheckForInterruptHang", mode: "off"}));
    let exitCode = awaitFind({checkExitSuccess: false});
    assert.neq(0, exitCode, "Expected shell to exit with failure due to operation kill");

    //
    // Snapshot getMores can be killed.
    //
    TestData.txnNumber++;

    // Start a getMore command that hangs before checking for interrupt.
    let awaitGetMore = startParallelShell(awaitGetMoreFn, rst.ports[0]);

    // Kill the command, and check that it is set to killPending.
    opId = waitForOpId({"originatingCommand.filter": {x: 1}});
    assert.commandWorked(db.killOp(opId));
    assertKillPending(opId);

    // Remove the hang, and check that the command is killed.
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "setCheckForInterruptHang", mode: "off"}));
    exitCode = awaitGetMore({checkExitSuccess: false});
    assert.neq(0, exitCode, "Expected shell to exit with failure due to operation kill");

    //
    // Snapshot finds do not yield locks.
    //
    TestData.txnNumber++;

    // Start a find command that hangs before checking for interrupt.
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "setCheckForInterruptHang", mode: "alwaysOn"}));
    awaitFind = startParallelShell(awaitFindFn, rst.ports[0]);
    waitForOpId({"command.filter": {x: 1}});

    // Start a drop. This should block behind the find, since the find does not yield locks.
    let awaitDrop = startParallelShell(function() {
        db.getSiblingDB("test").coll.drop();
    }, rst.ports[0]);

    // Remove the hang. The find should complete successfully.
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "setCheckForInterruptHang", mode: "off"}));
    awaitFind();

    // Now the drop can complete.
    awaitDrop();

    // Confirm that the find did not yield.
    let profilerEntry = getLatestProfilerEntry(db, {op: "query"});
    assert(profilerEntry.hasOwnProperty("numYield"), tojson(profilerEntry));
    assert.eq(0, profilerEntry.numYield, tojson(profilerEntry));

    // Restore the collection.
    for (let i = 0; i < 4; i++) {
        assert.commandWorked(db.coll.insert({_id: i, x: 1}, {writeConcern: {w: "majority"}}));
    }

    //
    // Snapshot getMores do not yield locks.
    //
    TestData.txnNumber++;

    // Start a getMore command that hangs before checking for interrupt.
    awaitGetMore = startParallelShell(awaitGetMoreFn, rst.ports[0]);
    waitForOpId({"originatingCommand.filter": {x: 1}});

    // Start a drop. This should block behing the getMore, since the getMore does not yield locks.
    awaitDrop = startParallelShell(function() {
        db.getSiblingDB("test").coll.drop();
    }, rst.ports[0]);

    // Remove the hang. The getMore should complete successfully.
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "setCheckForInterruptHang", mode: "off"}));
    awaitGetMore();

    // Now the drop can complete.
    awaitDrop();

    // Confirm that the getMore did not yield.
    profilerEntry = getLatestProfilerEntry(db, {op: "getmore"});
    assert(profilerEntry.hasOwnProperty("numYield"), tojson(profilerEntry));
    assert.eq(0, profilerEntry.numYield, tojson(profilerEntry));

    rst.stopSet();
}());
