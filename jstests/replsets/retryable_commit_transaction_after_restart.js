// Test committed transaction state is restored after restart.
// @tags: [uses_transactions, requires_persistence]
(function() {
"use strict";

load("jstests/replsets/rslib.js");

const dbName = "test";
const collName = "retryable_commit_transaction_after_restart";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const testDB = rst.getPrimary().getDB(dbName);
const testColl = testDB[collName];

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

assert.commandWorked(testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));
let txnNumber = 0;
let stmtId = 0;

const sessionOptions = {
    causalConsistency: false
};
const session = testDB.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);

jsTest.log("commitTransaction command is retryable before restart");
txnNumber++;
stmtId = 0;
assert.commandWorked(sessionDb.runCommand({
    insert: collName,
    documents: [{_id: "commit-txn-1"}],
    readConcern: {level: "snapshot"},
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(stmtId++),
    startTransaction: true,
    autocommit: false
}));
assert.commandWorked(sessionDb.adminCommand({
    commitTransaction: 1,
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(stmtId++),
    autocommit: false,
    writeConcern: {w: "majority"}
}));

// Retry commitTransaction.
assert.commandWorked(sessionDb.adminCommand({
    commitTransaction: 1,
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(stmtId),
    autocommit: false,
    writeConcern: {w: "majority"}
}));

jsTest.log("restart the single node replset");
rst.restart(0);
// Wait until the node becomes a primary and reconnect.
rst.getPrimary();
reconnect(sessionDb);

jsTest.log("commitTransaction command is retryable after restart");
// Retry commitTransaction.
assert.commandWorked(sessionDb.adminCommand({
    commitTransaction: 1,
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(stmtId),
    autocommit: false,
    writeConcern: {w: "majority"}
}));

jsTest.log("Attempt to abort a committed transaction after restart");
// Cannot abort the committed transaction.
assert.commandFailedWithCode(sessionDb.adminCommand({
    abortTransaction: 1,
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(stmtId++),
    autocommit: false,
    writeConcern: {w: "majority"}
}),
                             ErrorCodes.TransactionCommitted);

jsTest.log("Attempt to continue a committed transaction after restart");
assert.commandFailedWithCode(sessionDb.runCommand({
    insert: collName,
    documents: [{_id: "commit-txn-2"}],
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(stmtId++),
    autocommit: false
}),
                             ErrorCodes.TransactionCommitted);

session.endSession();
rst.stopSet();
}());
