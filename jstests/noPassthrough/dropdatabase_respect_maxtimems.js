/*
 * Tests that dropDatabase respects maxTimeMS.
 * @tags: [requires_replication, uses_transactions]
 */
(function() {
const rst = ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dropDB = rst.getPrimary().getDB("drop");

const waitForCommand = function(waitingFor, opFilter) {
    let opId = -1;
    assert.soon(function() {
        print(`Checking for ${waitingFor}`);
        const curopRes = dropDB.getSiblingDB("admin").currentOp();
        assert.commandWorked(curopRes);
        const foundOp = curopRes["inprog"].filter(opFilter);

        if (foundOp.length == 1) {
            opId = foundOp[0]["opid"];
        }
        return (foundOp.length == 1);
    });
    return opId;
};

(function assertCollectionDropCanBeInterrupted() {
    assert.commandWorked(dropDB.bar.insert({}));
    const session = dropDB.getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase("drop");
    session.startTransaction();
    assert.commandWorked(sessionDB.bar.insert({}));
    assert.commandFailedWithCode(dropDB.runCommand({dropDatabase: 1, maxTimeMS: 100}),
                                 ErrorCodes.MaxTimeMSExpired);

    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();
})();

(function assertDatabaseDropCanBeInterrupted() {
    load("jstests/libs/check_log.js");

    assert.commandWorked(dropDB.bar.insert({}));

    assert.commandWorked(rst.getPrimary().adminCommand(
        {configureFailPoint: "dropDatabaseHangAfterAllCollectionsDrop", mode: "alwaysOn"}));

    // This will get blocked by the failpoint when collection drop phase finishes.
    let dropDatabaseShell = startParallelShell(() => {
        assert.commandFailedWithCode(
            db.getSiblingDB("drop").runCommand({dropDatabase: 1, maxTimeMS: 5000}),
            ErrorCodes.MaxTimeMSExpired);
    }, rst.getPrimary().port);

    checkLog.contains(
        dropDB.getMongo(),
        "dropDatabase - fail point dropDatabaseHangAfterAllCollectionsDrop enabled. Blocking until fail point is disabled.");

    let sleepCommand = startParallelShell(() => {
        // Make dropDatabase timeout.
        assert.commandFailedWithCode(
            db.getSiblingDB("drop").adminCommand(
                {sleep: 1, secs: 500, lockTarget: "drop", lock: "ir", $comment: "Lock sleep"}),
            ErrorCodes.Interrupted);
    }, rst.getPrimary().port);

    checkLog.contains(dropDB.getMongo(), "test only command sleep invoked");

    // dropDatabase now gets unblocked by the failpoint but will immediately
    // get blocked by acquiring the database lock for dropping the database.
    assert.commandWorked(rst.getPrimary().adminCommand(
        {configureFailPoint: "dropDatabaseHangAfterAllCollectionsDrop", mode: "off"}));

    dropDatabaseShell();

    // Interrupt the sleep command.
    const sleepID = waitForCommand(
        "sleepCmd", op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == "Lock sleep"));
    assert.commandWorked(dropDB.getSiblingDB("admin").killOp(sleepID));

    sleepCommand();
})();

rst.stopSet();
})();
