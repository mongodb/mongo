/**
 * Test the following behavior for a transaction on step down.
 *  1) Active transactional operations (like read and write) are killed and the transaction is
 * aborted, but the connection not closed.
 *  2) Inactive transaction is aborted.
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    load("jstests/libs/curop_helpers.js");  // for waitForCurOpByFailPoint().

    const testName = "txnsDuringStepDown";
    const dbName = testName;
    const collName = "testcoll";

    const rst = new ReplSetTest(
        {nodes: [{setParameter: {closeConnectionsOnStepdown: false}}, {rsConfig: {priority: 0}}]});
    rst.startSet();
    rst.initiate();

    var primary = rst.getPrimary();
    var db = primary.getDB(dbName);
    var primaryAdmin = primary.getDB("admin");
    var primaryColl = db[collName];
    var collNss = primaryColl.getFullName();

    jsTestLog("Writing data to collection.");
    assert.writeOK(primaryColl.insert({_id: 'readOp'}, {"writeConcern": {"w": 2}}));

    TestData.dbName = dbName;
    TestData.collName = collName;
    TestData.skipRetryOnNetworkError = true;

    function startTxn({parallel: parallel = true}) {
        var txnFunc = () => {
            jsTestLog("Starting a new transaction.");
            const session = db.getMongo().startSession();
            const sessionDb = session.getDatabase(TestData.dbName);
            const sessionColl = sessionDb[TestData.collName];
            session.startTransaction({writeConcern: {w: "majority"}});
            print(TestData.cmd);
            eval(TestData.cmd);

            // Validate that the connection is not closed on step down.
            assert.commandWorked(db.adminCommand({ping: 1}));
        };
        return parallel ? startParallelShell(txnFunc, primary.port) : txnFunc();
    }

    function runStepDown() {
        jsTestLog("Making primary step down.");
        assert.commandWorked(primaryAdmin.runCommand({"replSetStepDown": 30 * 60, "force": true}));

        // Wait until the primary transitioned to SECONDARY state.
        rst.waitForState(primary, ReplSetTest.State.SECONDARY);

        jsTestLog("Validating data.");
        assert.docEq([{_id: 'readOp'}], primaryColl.find().toArray());

        jsTestLog("Making old primary eligible to be re-elected.");
        assert.commandWorked(primaryAdmin.runCommand({replSetFreeze: 0}));
        rst.getPrimary();
    }

    function testTxnFailsWithCode({
        op,
        failPoint: failPoint = 'hangAfterPreallocateSnapshot',
        nss: nss = dbName + '.$cmd',
        preOp: preOp = ''
    }) {
        jsTestLog("Enabling failPoint '" + failPoint + "' on primary.");
        assert.commandWorked(primary.adminCommand({
            configureFailPoint: failPoint,
            data: {shouldContinueOnInterrupt: true},
            mode: "alwaysOn"
        }));

        // Start transaction.
        TestData.cmd =
            preOp + `assert.commandFailedWithCode(${op}, ErrorCodes.InterruptedDueToStepDown);`;
        const waitForTxnShell = startTxn({});

        jsTestLog("Waiting for primary to reach failPoint '" + failPoint + "'.");
        waitForCurOpByFailPoint(primaryAdmin, nss, failPoint);

        // Call step down & validate data.
        runStepDown();

        // Wait for transaction shell to join.
        waitForTxnShell();

        // Disable fail point.
        assert.commandWorked(primaryAdmin.runCommand({configureFailPoint: failPoint, mode: 'off'}));
    }

    function testAbortOrCommitTxnFailsWithCode(params) {
        params["preOp"] = `sessionColl.insert({_id: 'abortOrCommitTxnOp'});`;
        params["nss"] = "admin.$cmd";
        testTxnFailsWithCode(params);
    }

    jsTestLog("Testing stepdown during read transaction.");
    testTxnFailsWithCode({op: "sessionDb.runCommand({find: '" + collName + "', batchSize: 1})"});

    jsTestLog("Testing stepdown during write transaction.");
    testTxnFailsWithCode({op: "sessionColl.insert({_id: 'writeOp'})"});

    jsTestLog("Testing stepdown during read-write transaction.");
    testTxnFailsWithCode({
        op: "sessionDb.runCommand({findAndModify: '" + collName +
            "', query: {_id: 'readOp'}, remove: true})"
    });

    jsTestLog("Testing stepdown during commit transaction.");
    testAbortOrCommitTxnFailsWithCode(
        {failPoint: "hangBeforeCommitingTxn", op: "session.commitTransaction_forTesting()"});

    jsTestLog("Testing stepdown during abort transaction.");
    testAbortOrCommitTxnFailsWithCode(
        {failPoint: "hangBeforeAbortingTxn", op: "session.abortTransaction_forTesting()"});

    jsTestLog("Testing stepdown during running transaction in inactive state.");
    TestData.cmd = "assert.writeOK(sessionColl.insert({_id: 'inactiveTxnOp'}))";
    // Do not start the transaction in parallel shell because when the parallel
    // shell work is done, implicit call to "endSessions" and "abortTransaction"
    // cmds are made. So, during step down we might not have any running
    // transaction to interrupt.
    startTxn({parallel: false});

    // Call step down & validate data.
    runStepDown();

    rst.stopSet();
})();
