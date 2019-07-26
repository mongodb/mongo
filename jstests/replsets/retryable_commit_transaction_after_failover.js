// Test committed transaction state is restored after failover.
// @tags: [uses_transactions]
(function() {
"use strict";

load("jstests/replsets/rslib.js");

const dbName = "test";
const collName = "retryable_commit_transaction_after_failover";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();

const config = rst.getReplSetConfig();
// Increase the election timeout so that we do not accidentally trigger an election while
// stepping up the old secondary.
config.settings = {
    "electionTimeoutMillis": 12 * 60 * 60 * 1000
};
rst.initiate(config);

// Get the connection to the replica set using MongoDB URI.
const conn = new Mongo(rst.getURL());
const testDB = conn.getDB(dbName);
const testColl = testDB[collName];

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

assert.commandWorked(testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));
let txnNumber = 0;
let stmtId = 0;

const sessionOptions = {
    causalConsistency: false
};
let session = testDB.getMongo().startSession(sessionOptions);
let sessionDb = session.getDatabase(dbName);

jsTest.log("commitTransaction command is retryable before failover");
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

jsTest.log("Step up the secondary");
const oldPrimary = rst.getPrimary();
const oldSecondary = rst.getSecondary();
rst.stepUp(oldSecondary);
// Wait until the other node becomes primary.
assert.eq(oldSecondary, rst.getPrimary());
// Reconnect the connection to the new primary.
sessionDb.getMongo()._markNodeAsFailed(
    oldPrimary.host, ErrorCodes.NotMaster, "Notice that primary is not master");
reconnect(sessionDb);

jsTest.log("commitTransaction command is retryable after failover");
// Retry commitTransaction.
assert.commandWorked(sessionDb.adminCommand({
    commitTransaction: 1,
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(stmtId),
    autocommit: false,
    writeConcern: {w: "majority"}
}));

jsTest.log("Attempt to abort a committed transaction after failover");
// Cannot abort the committed transaction.
assert.commandFailedWithCode(sessionDb.adminCommand({
    abortTransaction: 1,
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(stmtId++),
    autocommit: false,
    writeConcern: {w: "majority"}
}),
                             ErrorCodes.TransactionCommitted);

jsTest.log("Attempt to continue a committed transaction after failover");
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
