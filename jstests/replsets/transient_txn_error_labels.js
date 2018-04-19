// Test TransientTxnErrors error label in transactions.
// @tags: [uses_transactions]
(function() {
    "use strict";

    load("jstests/libs/write_concern_util.js");

    const dbName = "test";
    const collName = "no_error_labels_outside_txn";
    const rst = new ReplSetTest({name: collName, nodes: 2});
    const config = rst.getReplSetConfig();
    config.members[1].priority = 0;
    rst.startSet();
    rst.initiate(config);

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    const testDB = primary.getDB(dbName);
    const testColl = testDB.getCollection(collName);

    const sessionOptions = {causalConsistency: false};
    let session = secondary.startSession(sessionOptions);
    let sessionDb = session.getDatabase(dbName);
    let sessionColl = sessionDb.getCollection(collName);

    assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

    jsTest.log("Insert inside a transaction on secondary should fail but return error labels");
    let txnNumber = 0;
    let res = sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "insert-1"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    });
    assert.commandFailedWithCode(res, ErrorCodes.NotMaster);
    assert.eq(res.errorLabels, ["TransientTxnError"]);

    jsTest.log("Insert outside a transaction on secondary should fail but not return error labels");
    txnNumber++;
    // Insert as a retryable write.
    res = sessionDb.runCommand(
        {insert: collName, documents: [{_id: "insert-1"}], txnNumber: NumberLong(txnNumber)});

    assert.commandFailedWithCode(res, ErrorCodes.NotMaster);
    assert(!res.hasOwnProperty("errorLabels"));
    session.endSession();

    jsTest.log("Write concern errors should not have error labels");
    // Start a new session on the primary.
    session = primary.startSession(sessionOptions);
    sessionDb = session.getDatabase(dbName);
    sessionColl = sessionDb.getCollection(collName);
    stopServerReplication(secondary);
    session.startTransaction({writeConcern: {w: "majority", wtimeout: 1}});
    assert.commandWorked(sessionColl.insert({_id: "write-with-write-concern"}));
    res = session.commitTransaction_forTesting();
    assert.eq(res.writeConcernError.code, ErrorCodes.WriteConcernFailed);
    assert(!res.hasOwnProperty("code"));
    assert(!res.hasOwnProperty("errorLabels"));
    restartServerReplication(secondary);

    jsTest.log("failCommand should be able to return errors with TransientTxnError");
    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {errorCode: ErrorCodes.WriteConflict}
    }));
    session.startTransaction();
    jsTest.log("WriteCommandError should have error labels inside transactions.");
    res = sessionColl.insert({_id: "write-fail-point"});
    assert.commandFailedWithCode(res, ErrorCodes.WriteConflict);
    assert(res instanceof WriteCommandError);
    assert.eq(res.errorLabels, ["TransientTxnError"]);
    res = testColl.insert({_id: "write-fail-point-outside-txn"});
    jsTest.log("WriteCommandError should not have error labels outside transactions.");
    // WriteConflict will not be returned outside transactions in real cases, but it's fine for
    // testing purpose.
    assert.commandFailedWithCode(res, ErrorCodes.WriteConflict);
    assert(res instanceof WriteCommandError);
    assert(!res.hasOwnProperty("errorLabels"));
    assert.commandWorked(testDB.adminCommand({configureFailPoint: "failCommand", mode: "off"}));
    session.abortTransaction();

    jsTest.log("WriteConflict returned by commitTransaction command is TransientTxnError");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "commitTransaction-fail-point"}));
    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {errorCode: ErrorCodes.WriteConflict}
    }));
    res = session.commitTransaction_forTesting();
    assert.commandFailedWithCode(res, ErrorCodes.WriteConflict);
    assert.eq(res.errorLabels, ["TransientTxnError"]);
    assert.commandWorked(testDB.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    jsTest.log("NotMaster returned by commitTransaction command is not TransientTxnError");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "commitTransaction-fail-point"}));
    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {errorCode: ErrorCodes.NotMaster}
    }));
    res = session.commitTransaction_forTesting();
    assert.commandFailedWithCode(res, ErrorCodes.NotMaster);
    assert(!res.hasOwnProperty("errorLabels"));
    assert.commandWorked(testDB.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    jsTest.log("ShutdownInProgress returned by write commands is TransientTxnError");
    session.startTransaction();
    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {errorCode: ErrorCodes.ShutdownInProgress}
    }));
    res = sessionColl.insert({_id: "commitTransaction-fail-point"});
    assert.commandFailedWithCode(res, ErrorCodes.ShutdownInProgress);
    assert(res instanceof WriteCommandError);
    assert.eq(res.errorLabels, ["TransientTxnError"]);
    assert.commandWorked(testDB.adminCommand({configureFailPoint: "failCommand", mode: "off"}));
    session.abortTransaction();

    jsTest.log("ShutdownInProgress returned by commitTransaction command is not TransientTxnError");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "commitTransaction-fail-point"}));
    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {errorCode: ErrorCodes.ShutdownInProgress}
    }));
    res = session.commitTransaction_forTesting();
    assert.commandFailedWithCode(res, ErrorCodes.ShutdownInProgress);
    assert(!res.hasOwnProperty("errorLabels"));
    assert.commandWorked(testDB.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    // The fail-point leaves the transaction open so we need to abort it explicitly.
    // TODO: after SERVER-34795, endSession() will kill the running transactions.
    session.startTransaction();
    assert.docEq(sessionColl.find().toArray(), [{"_id": "write-with-write-concern"}]);
    session.abortTransaction();

    session.endSession();

    rst.stopSet();
}());
