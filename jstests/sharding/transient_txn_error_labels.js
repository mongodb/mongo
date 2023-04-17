/**
 * Test TransientTransactionErrors error label in transactions.
 * @tags: [
 *   uses_transactions,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/write_concern_util.js");
load("jstests/libs/parallelTester.js");  // For Thread.

const dbName = "test";
const collName = "no_error_labels_outside_txn";

// We are testing coordinateCommitTransaction, which requires the nodes to be started with
// --shardsvr.
const st = new ShardingTest({
    config: TestData.configShard ? undefined : 1,
    mongos: 1,
    shards: {rs0: {nodes: [{}, {rsConfig: {priority: 0}}]}}
});
const primary = st.rs0.getPrimary();
const secondary = st.rs0.getSecondary();

const testDB = primary.getDB(dbName);
const adminDB = testDB.getSiblingDB("admin");
const testColl = testDB.getCollection(collName);

const sessionOptions = {
    causalConsistency: false
};
let session = primary.startSession(sessionOptions);
let sessionDb = session.getDatabase(dbName);
let sessionColl = sessionDb.getCollection(collName);
let secondarySession = secondary.startSession(sessionOptions);
let secondarySessionDb = secondarySession.getDatabase(dbName);

assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

jsTest.log("Insert inside a transaction on secondary should fail but return error labels");
let txnNumber = 0;
let res = secondarySessionDb.runCommand({
    insert: collName,
    documents: [{_id: "insert-1"}],
    readConcern: {level: "snapshot"},
    txnNumber: NumberLong(txnNumber),
    startTransaction: true,
    autocommit: false
});
assert.commandFailedWithCode(res, ErrorCodes.NotWritablePrimary);
assert.eq(res.errorLabels, ["TransientTransactionError"]);

jsTest.log("failCommand with errorLabels but without errorCode or writeConcernError should not " +
           "interfere with server's error labels attaching");
txnNumber++;
// This failCommand should have no effect.
assert.commandWorked(secondary.adminCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {errorLabels: ["foo"], failCommands: ["insert"]}
}));
res = secondarySessionDb.runCommand({
    insert: collName,
    documents: [{_id: "insert-1"}],
    readConcern: {level: "snapshot"},
    txnNumber: NumberLong(txnNumber),
    startTransaction: true,
    autocommit: false
});
assert.commandFailedWithCode(res, ErrorCodes.NotWritablePrimary);
// Server should continue to return TransientTransactionError label.
assert.eq(res.errorLabels, ["TransientTransactionError"]);
assert.commandWorked(secondary.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

jsTest.log("Insert as a retryable write on secondary should fail with retryable error labels");
txnNumber++;
// Insert as a retryable write.
res = secondarySessionDb.runCommand(
    {insert: collName, documents: [{_id: "insert-1"}], txnNumber: NumberLong(txnNumber)});

assert.commandFailedWithCode(res, ErrorCodes.NotWritablePrimary);
assert.eq(res.errorLabels, ["RetryableWriteError"], res);
secondarySession.endSession();

jsTest.log("failCommand should be able to return errors with TransientTransactionError");
assert.commandWorked(testDB.adminCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {errorCode: ErrorCodes.WriteConflict, failCommands: ["insert"]}
}));
session.startTransaction();
jsTest.log("WriteCommandError should have error labels inside transactions.");
res = sessionColl.insert({_id: "write-fail-point"});
assert.commandFailedWithCode(res, ErrorCodes.WriteConflict);
assert(res instanceof WriteCommandError);
assert.eq(res.errorLabels, ["TransientTransactionError"]);
res = testColl.insert({_id: "write-fail-point-outside-txn"});
jsTest.log("WriteCommandError should not have error labels outside transactions.");
// WriteConflict will not be returned outside transactions in real cases, but it's fine for
// testing purpose.
assert.commandFailedWithCode(res, ErrorCodes.WriteConflict);
assert(res instanceof WriteCommandError);
assert(!res.hasOwnProperty("errorLabels"));
assert.commandWorked(testDB.adminCommand({configureFailPoint: "failCommand", mode: "off"}));
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

jsTest.log("WriteConflict returned by commitTransaction command is TransientTransactionError");
session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: "commitTransaction-fail-point"}));
assert.commandWorked(testDB.adminCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {errorCode: ErrorCodes.WriteConflict, failCommands: ["commitTransaction"]}
}));
res = session.commitTransaction_forTesting();
assert.commandFailedWithCode(res, ErrorCodes.WriteConflict);
assert.eq(res.errorLabels, ["TransientTransactionError"]);
assert.commandWorked(testDB.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

jsTest.log(
    "NotWritablePrimary returned by commitTransaction command is not TransientTransactionError but" +
    " RetryableWriteError");
// commitTransaction will attempt to perform a noop write in response to a NoSuchTransaction
// error and non-empty writeConcern. This will throw NotWritablePrimary.
res = secondarySessionDb.adminCommand({
    commitTransaction: 1,
    txnNumber: NumberLong(secondarySession.getTxnNumber_forTesting() + 1),
    autocommit: false,
    writeConcern: {w: "majority"}
});
assert.commandFailedWithCode(res, ErrorCodes.NotWritablePrimary);
assert.eq(res.errorLabels, ["RetryableWriteError"], res);

jsTest.log(
    "NotWritablePrimary returned by coordinateCommitTransaction command is not TransientTransactionError" +
    " but RetryableWriteError");
// coordinateCommitTransaction will attempt to perform a noop write in response to a
// NoSuchTransaction error and non-empty writeConcern. This will throw NotWritablePrimary.
res = secondarySessionDb.adminCommand({
    coordinateCommitTransaction: 1,
    participants: [],
    txnNumber: NumberLong(secondarySession.getTxnNumber_forTesting() + 1),
    autocommit: false,
    writeConcern: {w: "majority"}
});
assert.commandFailedWithCode(res, ErrorCodes.NotWritablePrimary);
assert.eq(res.errorLabels, ["RetryableWriteError"], res);

jsTest.log("ShutdownInProgress returned by write commands is TransientTransactionError");
session.startTransaction();
assert.commandWorked(testDB.adminCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {errorCode: ErrorCodes.ShutdownInProgress, failCommands: ["insert"]}
}));
res = sessionColl.insert({_id: "commitTransaction-fail-point"});
assert.commandFailedWithCode(res, ErrorCodes.ShutdownInProgress);
assert(res instanceof WriteCommandError);
assert.eq(res.errorLabels, ["TransientTransactionError"]);
assert.commandWorked(testDB.adminCommand({configureFailPoint: "failCommand", mode: "off"}));
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

jsTest.log(
    "ShutdownInProgress returned by commitTransaction command is not TransientTransactionError" +
    " but RetryableWriteError");
session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: "commitTransaction-fail-point"}));
assert.commandWorked(testDB.adminCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {errorCode: ErrorCodes.ShutdownInProgress, failCommands: ["commitTransaction"]}
}));
res = session.commitTransaction_forTesting();
assert.commandFailedWithCode(res, ErrorCodes.ShutdownInProgress);
assert.eq(res.errorLabels, ["RetryableWriteError"], res);
assert.commandWorked(testDB.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

jsTest.log("ShutdownInProgress returned by coordinateCommitTransaction command is not" +
           " TransientTransactionError but RetryableWriteError");
session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: "coordinateCommitTransaction-fail-point"}));
assert.commandWorked(testDB.adminCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {errorCode: ErrorCodes.ShutdownInProgress, failCommands: ["coordinateCommitTransaction"]}
}));
res = sessionDb.adminCommand({
    coordinateCommitTransaction: 1,
    participants: [],
    txnNumber: NumberLong(session.getTxnNumber_forTesting()),
    autocommit: false
});
assert.commandFailedWithCode(res, ErrorCodes.ShutdownInProgress);
assert.eq(res.errorLabels, ["RetryableWriteError"], res);
assert.commandWorked(session.abortTransaction_forTesting());
assert.commandWorked(testDB.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

jsTest.log("LockTimeout should be TransientTransactionError");
// Start a transaction to hold the DBLock in IX mode so that drop will be blocked.
session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: "lock-timeout-1"}));
function dropCmdFunc(primaryHost, dbName, collName) {
    const primary = new Mongo(primaryHost);
    return primary.getDB(dbName).runCommand({drop: collName, writeConcern: {w: "majority"}});
}
const thread = new Thread(dropCmdFunc, primary.host, dbName, collName);
thread.start();
// Wait for the drop to have a pending MODE_X lock on the database.
assert.soon(
    function() {
        return adminDB
                   .aggregate([
                       {$currentOp: {}},
                       {$match: {"command.drop": collName, waitingForLock: true}}
                   ])
                   .itcount() === 1;
    },
    function() {
        return "Failed to find drop in currentOp output: " +
            tojson(adminDB.aggregate([{$currentOp: {}}]).toArray());
    });
// Start another transaction in a new session, which cannot acquire the database lock in time.
let sessionOther = primary.startSession(sessionOptions);
sessionOther.startTransaction();
res = sessionOther.getDatabase(dbName).getCollection(collName).insert({_id: "lock-timeout-2"});
assert.commandFailedWithCode(res, ErrorCodes.LockTimeout);
assert(res instanceof WriteCommandError);
assert.eq(res.errorLabels, ["TransientTransactionError"]);
assert.commandFailedWithCode(sessionOther.abortTransaction_forTesting(),
                             ErrorCodes.NoSuchTransaction);
assert.commandWorked(session.abortTransaction_forTesting());
thread.join();
assert.commandWorked(thread.returnData());

// Re-create the collection for later test cases.
assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

jsTest.log("Network errors for in-progress statements should be transient");
session.startTransaction();
assert.commandWorked(testDB.adminCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {errorCode: ErrorCodes.HostUnreachable, failCommands: ["aggregate"]}
}));
res = sessionDb.runCommand({aggregate: collName, pipeline: [{$match: {}}], cursor: {}});
assert.commandFailedWithCode(res, ErrorCodes.HostUnreachable);
assert.eq(res.errorLabels, ["TransientTransactionError"]);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
assert.commandWorked(testDB.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

jsTest.log("Network errors for commit should not be transient but RetryableWriteError");
session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: "commitTransaction-network-error"}));
assert.commandWorked(testDB.adminCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {errorCode: ErrorCodes.HostUnreachable, failCommands: ["commitTransaction"]}
}));
res = sessionDb.adminCommand({
    commitTransaction: 1,
    txnNumber: NumberLong(session.getTxnNumber_forTesting()),
    autocommit: false
});
assert.commandFailedWithCode(res, ErrorCodes.HostUnreachable);
assert.eq(res.errorLabels, ["RetryableWriteError"], res);
assert.commandWorked(session.abortTransaction_forTesting());
assert.commandWorked(testDB.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

session.endSession();

st.stop();
}());
