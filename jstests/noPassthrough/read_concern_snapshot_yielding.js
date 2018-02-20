// Test that the read concern level 'snapshot' exhibits the correct yielding behavior. That is,
// operations performed at read concern level snapshot check for interrupt but do not yield locks or
// storage engine resources.
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
    TestData.numDocs = 4;

    if (!db.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        rst.stopSet();
        return;
    }

    TestData.sessionId = assert.commandWorked(adminDB.runCommand({startSession: 1})).id;
    TestData.txnNumber = 0;

    // Set 'internalQueryExecYieldIterations' to 2 to ensure that commands yield on the second try
    // (i.e. after they have established a snapshot but before they have returned any documents).
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 2}));
    db.setProfilingLevel(2);

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

    function populateCollection() {
        db.coll.drop();
        for (let i = 0; i < TestData.numDocs; i++) {
            assert.commandWorked(db.coll.insert({_id: i, x: 1}, {writeConcern: {w: "majority"}}));
        }
    }

    function testCommand(awaitCommandFn, curOpFilter, profilerFilter, testWriteConflict) {
        //
        // Test that the command can be killed.
        //

        TestData.txnNumber++;
        populateCollection();

        // Start a command that hangs before checking for interrupt.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "alwaysOn"}));
        let awaitCommand = startParallelShell(awaitCommandFn, rst.ports[0]);

        // Kill the command, and check that it is set to killPending.
        let opId = waitForOpId(curOpFilter);
        assert.commandWorked(db.killOp(opId));
        assertKillPending(opId);

        // Remove the hang, and check that the command is killed.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
        let exitCode = awaitCommand({checkExitSuccess: false});
        assert.neq(0, exitCode, "Expected shell to exit with failure due to operation kill");

        //
        // Test that the command does not yield locks.
        //

        TestData.txnNumber++;
        populateCollection();

        // Start a command that hangs before checking for interrupt.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "alwaysOn"}));
        awaitCommand = startParallelShell(awaitCommandFn, rst.ports[0]);
        waitForOpId(curOpFilter);

        // Start a drop. This should block behind the command, since the command does not yield
        // locks.
        let awaitDrop = startParallelShell(function() {
            db.getSiblingDB("test").coll.drop();
        }, rst.ports[0]);

        // Remove the hang. The command should complete successfully.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
        awaitCommand();

        // Now the drop can complete.
        awaitDrop();

        // Confirm that the command did not yield.
        if (profilerFilter) {
            let profilerEntry = getLatestProfilerEntry(db, profilerFilter);
            assert(profilerEntry.hasOwnProperty("numYield"), tojson(profilerEntry));
            assert.eq(0, profilerEntry.numYield, tojson(profilerEntry));
        }

        //
        // Test that the command does not read data that is inserted during its execution.
        // 'awaitCommandFn' should fail if it reads a document {_id: <numDocs>, x: 1, new: 1}.
        //

        TestData.txnNumber++;
        populateCollection();

        // Start a command that hangs before checking for interrupt.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "alwaysOn"}));
        awaitCommand = startParallelShell(awaitCommandFn, rst.ports[0]);
        waitForOpId(curOpFilter);

        // Insert data that should not be read by the command.
        assert.commandWorked(
            db.coll.insert({_id: TestData.numDocs, x: 1, new: 1}, {writeConcern: {w: "majority"}}));

        // Remove the hang. The command should complete successfully.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
        awaitCommand();

        //
        // Test that the command fails if a write conflict occurs. 'awaitCommandFn' should write to
        // {_id: <numDocs>, x: 1, new: 1}.
        //

        if (testWriteConflict) {
            TestData.txnNumber++;
            populateCollection();

            // Insert the document that the command will write to.
            assert.commandWorked(db.coll.insert({_id: TestData.numDocs, x: 1, new: 1},
                                                {writeConcern: {w: "majority"}}));

            // Start a command that hangs before checking for interrupt.
            assert.commandWorked(db.adminCommand({
                configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang",
                mode: "alwaysOn"
            }));
            awaitCommand = startParallelShell(awaitCommandFn, rst.ports[0]);
            waitForOpId(curOpFilter);

            // Update the document that the command will write to.
            assert.commandWorked(db.coll.update({_id: TestData.numDocs},
                                                {$set: {conflict: true}},
                                                {writeConcern: {w: "majority"}}));

            // Remove the hang. The command should fail.
            assert.commandWorked(db.adminCommand(
                {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
            exitCode = awaitCommand({checkExitSuccess: false});
            assert.neq(0, exitCode, "Expected shell to exit with failure due to WriteConflict");
        }
    }

    // Test find.
    testCommand(function() {
        const res = assert.commandWorked(db.runCommand({
            find: "coll",
            filter: {x: 1},
            readConcern: {level: "snapshot"},
            lsid: TestData.sessionId,
            txnNumber: NumberLong(TestData.txnNumber)
        }));
        assert.eq(res.cursor.firstBatch.length, TestData.numDocs, tojson(res));
    }, {"command.filter": {x: 1}}, {op: "query"});

    // Test getMore.
    testCommand(function() {
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
        const initialFindBatchSize = 2;
        const cursorId = assert
                             .commandWorked(db.runCommand({
                                 find: "coll",
                                 filter: {x: 1},
                                 batchSize: initialFindBatchSize,
                                 readConcern: {level: "snapshot"},
                                 lsid: TestData.sessionId,
                                 txnNumber: NumberLong(TestData.txnNumber)
                             }))
                             .cursor.id;
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "alwaysOn"}));
        const res = assert.commandWorked(db.runCommand({
            getMore: NumberLong(cursorId),
            collection: "coll",
            batchSize: TestData.numDocs,
            lsid: TestData.sessionId,
            txnNumber: NumberLong(TestData.txnNumber)
        }));
        assert.eq(
            res.cursor.nextBatch.length, TestData.numDocs - initialFindBatchSize, tojson(res));
    }, {"originatingCommand.filter": {x: 1}}, {op: "getmore"});

    // Test update.
    // We cannot profide a 'profilerFilter' because profiling is turned off for write commands in
    // transactions.
    testCommand(function() {
        const res = assert.commandWorked(db.runCommand({
            update: "coll",
            updates: [{q: {new: 1}, u: {$set: {updated: true}}}],
            readConcern: {level: "snapshot"},
            lsid: TestData.sessionId,
            txnNumber: NumberLong(TestData.txnNumber)
        }));
        assert.eq(res.n, 0, tojson(res));
        assert.eq(res.nModified, 0, tojson(res));
    }, {op: "update"}, null, true);

    rst.stopSet();
}());
