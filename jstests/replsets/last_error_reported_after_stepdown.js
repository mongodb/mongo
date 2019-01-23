/**
 * Tests that writes which complete before stepdown correctly report their errors after the
 * stepdown.
 */
(function() {
    "use strict";

    load("jstests/libs/check_log.js");

    const rst = new ReplSetTest(
        {nodes: [{setParameter: {closeConnectionsOnStepdown: false}}, {rsConfig: {priority: 0}}]});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const primaryAdmin = primary.getDB("admin");
    // We need a separate connection to avoid interference with the ReplSetTestMechanism.
    const primaryDataConn = new Mongo(primary.host);
    const primaryDb = primaryDataConn.getDB("test");
    const collname = "last_error_reported_after_stepdown";
    const coll = primaryDb[collname];

    // Never retry on network error, because this test needs to detect the network error.
    TestData.skipRetryOnNetworkError = true;

    // This is specifically testing unacknowledged legacy writes.
    primaryDataConn.forceWriteMode('legacy');

    assert.commandWorked(
        coll.insert([{_id: 'deleteme'}, {_id: 'updateme', nullfield: null}, {_id: 'findme'}],
                    {writeConcern: {w: 1}}));
    rst.awaitReplication();

    // Note that "operation" should always be on primaryDataConn, so the stepdown doesn't clear
    // the last error.
    function runStepDownTest({description, operation, errorCode, nDocs}) {
        jsTestLog(`Trying ${description} on the primary, then stepping down`);
        operation();
        // Wait for the operation to complete.
        assert.soon(
            () => primaryAdmin.aggregate([{'$currentOp': {}}, {'$match': {ns: coll.getName()}}])
                      .itcount() == 0);
        assert.commandWorked(primaryAdmin.adminCommand({replSetStepDown: 60, force: true}));
        rst.waitForState(primary, ReplSetTest.State.SECONDARY);
        var lastError = assert.commandWorked(primaryDb.runCommand({getLastError: 1}));
        if (typeof(errorCode) == "number")
            assert.eq(lastError.code,
                      errorCode,
                      "Expected error code " + errorCode + ", got lastError of " +
                          JSON.stringify(lastError));
        else {
            assert(!lastError.err,
                   "Expected no error, got lastError of " + JSON.stringify(lastError));
        }
        if (typeof(nDocs) == "number") {
            assert.eq(lastError.n, nDocs, "Wrong number of documents modified or updated");
        }

        // Allow the primary to be re-elected, and wait for it.
        assert.commandWorked(primaryAdmin.adminCommand({replSetFreeze: 0}));
        rst.getPrimary();
    }

    // Tests which should have no errors.
    runStepDownTest({description: "insert", operation: () => coll.insert({_id: 0})});
    runStepDownTest({
        description: "update",
        operation: () => coll.update({_id: 'updateme'}, {'$inc': {x: 1}}),
        nDocs: 1
    });
    runStepDownTest(
        {description: "remove", operation: () => coll.remove({_id: 'deleteme'}), nDocs: 1});

    // Tests which should have errors.
    runStepDownTest({
        description: "insert with error",
        operation: () => coll.insert({_id: 0}),
        errorCode: ErrorCodes.DuplicateKey
    });
    runStepDownTest({
        description: "update with error",
        operation: () => coll.update({_id: 'updateme'}, {'$inc': {nullfield: 1}}),
        errorCode: ErrorCodes.TypeMismatch,
        nDocs: 0
    });
    runStepDownTest({
        description: "remove with error",
        operation: () => coll.remove({'$nonsense': {x: 1}}),
        errorCode: ErrorCodes.BadValue,
        nDocs: 0
    });

    rst.stopSet();
})();
