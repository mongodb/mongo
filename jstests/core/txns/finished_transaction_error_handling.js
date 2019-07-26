// Test committed and aborted transactions cannot be changed but commitTransaction is retryable.
// @tags: [uses_transactions, uses_snapshot_read_concern]
(function() {
"use strict";

const dbName = "test";
const collName = "finished_transaction_error_handling";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB[collName];

const writeConcern = {
    w: "majority",
    wtimeout: ReplSetTest.kDefaultTimeoutMS
};
testDB.runCommand({drop: collName, writeConcern: writeConcern});
assert.commandWorked(testDB.createCollection(collName, {writeConcern: writeConcern}));

let txnNumber = 0;
let stmtId = 0;

const sessionOptions = {
    causalConsistency: false
};
const session = db.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);

jsTestLog("Test aborted transaction number cannot be reused.");
txnNumber++;
assert.commandWorked(sessionDb.runCommand({
    insert: collName,
    documents: [{_id: "abort-txn-1"}],
    readConcern: {level: "snapshot"},
    txnNumber: NumberLong(txnNumber),
    startTransaction: true,
    stmtId: NumberInt(stmtId++),
    autocommit: false
}));
assert.commandWorked(sessionDb.adminCommand({
    abortTransaction: 1,
    writeConcern: {w: "majority"},
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(stmtId++),
    autocommit: false
}));

jsTestLog("Attempt to commit an aborted transaction");
assert.commandFailedWithCode(sessionDb.adminCommand({
    commitTransaction: 1,
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(stmtId++),
    autocommit: false
}),
                             ErrorCodes.NoSuchTransaction);

jsTestLog("Attempt to abort an aborted transaction");
assert.commandFailedWithCode(sessionDb.adminCommand({
    abortTransaction: 1,
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(stmtId++),
    autocommit: false
}),
                             ErrorCodes.NoSuchTransaction);

jsTestLog("Attempt to continue an aborted transaction");
assert.commandFailedWithCode(sessionDb.runCommand({
    insert: collName,
    documents: [{_id: "abort-txn-2"}],
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(stmtId++),
    autocommit: false
}),
                             ErrorCodes.NoSuchTransaction);

jsTestLog("Attempt to restart an aborted transaction");
assert.commandFailedWithCode(sessionDb.runCommand({
    insert: collName,
    documents: [{_id: "abort-txn-2"}],
    readConcern: {level: "snapshot"},
    txnNumber: NumberLong(txnNumber),
    startTransaction: true,
    stmtId: NumberInt(stmtId++),
    autocommit: false
}),
                             ErrorCodes.ConflictingOperationInProgress);

jsTest.log("Test commitTransaction command is retryable");
txnNumber++;
stmtId = 0;
assert.commandWorked(sessionDb.runCommand({
    insert: collName,
    documents: [{_id: "commit-txn-1"}],
    readConcern: {level: "snapshot"},
    txnNumber: NumberLong(txnNumber),
    startTransaction: true,
    stmtId: NumberInt(stmtId++),
    autocommit: false
}));
assert.commandWorked(sessionDb.adminCommand({
    commitTransaction: 1,
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(stmtId++),
    autocommit: false
}));

jsTestLog("Retry commitTransaction command on a committed transaction");
assert.commandWorked(sessionDb.adminCommand({
    commitTransaction: 1,
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(stmtId),
    autocommit: false
}));

jsTestLog("Attempt to abort a committed transaction");
assert.commandFailedWithCode(sessionDb.adminCommand({
    abortTransaction: 1,
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(stmtId++),
    autocommit: false
}),
                             ErrorCodes.TransactionCommitted);

jsTestLog("Attempt to continue a committed transaction");
assert.commandFailedWithCode(sessionDb.runCommand({
    insert: collName,
    documents: [{_id: "commit-txn-2"}],
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(stmtId++),
    autocommit: false
}),
                             ErrorCodes.TransactionCommitted);

jsTestLog("Attempt to restart a committed transaction");
assert.commandFailedWithCode(sessionDb.runCommand({
    insert: collName,
    documents: [{_id: "commit-txn-2"}],
    readConcern: {level: "snapshot"},
    txnNumber: NumberLong(txnNumber),
    startTransaction: true,
    stmtId: NumberInt(stmtId++),
    autocommit: false
}),
                             ErrorCodes.ConflictingOperationInProgress);

session.endSession();
}());
