/*
 * Tests that dropDatabase respects maxTimeMS.
 * @tags: [requires_replication, uses_transactions]
 */
(function() {
    const rst = ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const adminDB = rst.getPrimary().getDB("admin");
    const txnDB = rst.getPrimary().getDB("txn");
    const dropDB = rst.getPrimary().getDB("drop");

    (function assertColletionDropCanBeInterrupted() {
        assert.commandWorked(txnDB.foo.insert({}));
        assert.commandWorked(dropDB.bar.insert({}));
        const session = txnDB.getMongo().startSession({causalConsistency: false});
        const sessionDB = session.getDatabase("txn");
        session.startTransaction();
        assert.commandWorked(sessionDB.foo.insert({}));
        assert.commandFailedWithCode(dropDB.runCommand({dropDatabase: 1, maxTimeMS: 100}),
                                     ErrorCodes.MaxTimeMSExpired);

        assert.commandWorked(session.commitTransaction_forTesting());
        session.endSession();
    })();

    (function assertDatabaseDropCanBeInterrupted() {
        assert.commandWorked(txnDB.foo.insert({}));
        assert.commandWorked(dropDB.bar.insert({}));

        assert.commandWorked(rst.getPrimary().adminCommand(
            {configureFailPoint: "dropDatabaseHangAfterAllCollectionsDrop", mode: "alwaysOn"}));

        // This will get blocked by the failpoint when collection drop phase finishes.
        let dropDatabaseShell = startParallelShell(
            "assert.commandFailedWithCode(db.getSiblingDB(\"drop\").runCommand({dropDatabase: 1, maxTimeMS: 5000}), ErrorCodes.MaxTimeMSExpired);",
            rst.getPrimary().port);

        assert.soon(function() {
            const sessionFilter = {active: true, "command.dropDatabase": 1};
            const res = adminDB.aggregate([{$currentOp: {}}, {$match: sessionFilter}]);
            return res.hasNext();
        }, "Timeout waiting for dropDatabase to start");

        const session = txnDB.getMongo().startSession({causalConsistency: false});
        const sessionDB = session.getDatabase("txn");
        session.startTransaction();
        assert.commandWorked(sessionDB.foo.insert({}));

        // dropDatabase now gets unblocked by the failpoint but will immediately
        // get blocked by acquiring the GlobalWrite lock for dropping the database.
        assert.commandWorked(rst.getPrimary().adminCommand(
            {configureFailPoint: "dropDatabaseHangAfterAllCollectionsDrop", mode: "off"}));

        // This should timeout.
        dropDatabaseShell();

        assert.commandWorked(session.commitTransaction_forTesting());
        session.endSession();
    })();

    rst.stopSet();
})();
