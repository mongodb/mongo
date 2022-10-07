/*
 * Tests that dropDatabase respects maxTimeMS.
 * @tags: [requires_replication, uses_transactions]
 */
(function() {
load("jstests/libs/fail_point_util.js");
load("jstests/libs/wait_for_command.js");
const rst = ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dropDB = rst.getPrimary().getDB("drop");

(function assertCollectionDropCanBeInterrupted() {
    assert.commandWorked(dropDB.bar.insert({_id: 0}, {writeConcern: {w: 'majority'}}));
    const session = dropDB.getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase("drop");
    session.startTransaction();
    assert.commandWorked(sessionDB.bar.insert({_id: 1}));
    assert.commandFailedWithCode(dropDB.runCommand({dropDatabase: 1, maxTimeMS: 100}),
                                 ErrorCodes.MaxTimeMSExpired);

    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();
})();

(function assertDatabaseDropCanBeInterrupted() {
    assert.commandWorked(dropDB.bar.insert({}));

    const failPoint =
        configureFailPoint(rst.getPrimary(), "dropDatabaseHangAfterAllCollectionsDrop");

    // This will get blocked by the failpoint when collection drop phase finishes.
    let dropDatabaseShell = startParallelShell(() => {
        assert.commandFailedWithCode(
            db.getSiblingDB("drop").runCommand({dropDatabase: 1, maxTimeMS: 5000}),
            ErrorCodes.MaxTimeMSExpired);
    }, rst.getPrimary().port);

    failPoint.wait();

    let sleepCommand = startParallelShell(() => {
        // Make dropDatabase timeout.
        assert.commandFailedWithCode(
            db.getSiblingDB("drop").adminCommand(
                {sleep: 1, secs: 500, lockTarget: "drop", lock: "ir", $comment: "Lock sleep"}),
            ErrorCodes.Interrupted);
    }, rst.getPrimary().port);

    checkLog.contains(dropDB.getMongo(), "Test-only command 'sleep' invoked");

    // dropDatabase now gets unblocked by the failpoint but will immediately
    // get blocked by acquiring the database lock for dropping the database.
    failPoint.off();

    dropDatabaseShell();

    // Interrupt the sleep command.
    const sleepID = waitForCommand(
        "sleepCmd",
        op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == "Lock sleep"),
        dropDB.getSiblingDB("admin"));
    assert.commandWorked(dropDB.getSiblingDB("admin").killOp(sleepID));

    sleepCommand();
})();

rst.stopSet();
})();
