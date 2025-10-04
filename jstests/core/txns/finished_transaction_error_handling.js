// Test committed and aborted transactions cannot be changed but commitTransaction is retryable.
//
// @tags: [
//  # The test runs commands that are not allowed with security token: endSession.
//  not_allowed_with_signed_security_token,
//  uses_transactions,
//  uses_snapshot_read_concern,
//]
// TODO (SERVER-39704): Remove the following load after SERVER-39704 is completed
import {retryOnceOnTransientOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "test";
const collName = "finished_transaction_error_handling";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB[collName];

const writeConcern = {
    w: "majority",
    wtimeout: ReplSetTest.kDefaultTimeoutMS,
};
testDB.runCommand({drop: collName, writeConcern: writeConcern});
assert.commandWorked(testDB.createCollection(collName, {writeConcern: writeConcern}));

let txnNumber = 0;
let stmtId = 0;

const sessionOptions = {
    causalConsistency: false,
};
const session = db.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);

jsTestLog("Test aborted transaction number cannot be reused.");

// TODO (SERVER-39704): We use the retryOnceOnTransientOnMongos
// function to handle how MongoS will propagate a StaleShardVersion error as a
// TransientTransactionError. After SERVER-39704 is completed the
// retryOnceOnTransientOnMongos can be removed
retryOnceOnTransientOnMongos(session, () => {
    assert.commandWorked(
        sessionDb.runCommand({
            insert: collName,
            documents: [{_id: "abort-txn-1"}],
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(++txnNumber),
            startTransaction: true,
            stmtId: NumberInt(stmtId++),
            autocommit: false,
        }),
    );
});

assert.commandWorked(
    sessionDb.adminCommand({
        abortTransaction: 1,
        writeConcern: {w: "majority"},
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++),
        autocommit: false,
    }),
);

jsTestLog("Attempt to commit an aborted transaction");
assert.commandFailedWithCode(
    sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++),
        autocommit: false,
    }),
    ErrorCodes.NoSuchTransaction,
);

jsTestLog("Attempt to abort an aborted transaction");
assert.commandFailedWithCode(
    sessionDb.adminCommand({
        abortTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++),
        autocommit: false,
    }),
    ErrorCodes.NoSuchTransaction,
);

jsTestLog("Attempt to continue an aborted transaction");
assert.commandFailedWithCode(
    sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "abort-txn-2"}],
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++),
        autocommit: false,
    }),
    ErrorCodes.NoSuchTransaction,
);

jsTestLog("Attempt to restart an aborted transaction");
assert.commandFailedWithCode(
    sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "abort-txn-2"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        stmtId: NumberInt(stmtId++),
        autocommit: false,
    }),
    ErrorCodes.ConflictingOperationInProgress,
);

jsTest.log("Test commitTransaction command is retryable");
txnNumber++;
stmtId = 0;
assert.commandWorked(
    sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "commit-txn-1"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        stmtId: NumberInt(stmtId++),
        autocommit: false,
    }),
);
assert.commandWorked(
    sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++),
        autocommit: false,
    }),
);

jsTestLog("Retry commitTransaction command on a committed transaction");
assert.commandWorked(
    sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId),
        autocommit: false,
    }),
);

jsTestLog("Attempt to abort a committed transaction");
assert.commandFailedWithCode(
    sessionDb.adminCommand({
        abortTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++),
        autocommit: false,
    }),
    ErrorCodes.TransactionCommitted,
);

jsTestLog("Attempt to continue a committed transaction");
assert.commandFailedWithCode(
    sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "commit-txn-2"}],
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++),
        autocommit: false,
    }),
    ErrorCodes.TransactionCommitted,
);

jsTestLog("Attempt to restart a committed transaction");
assert.commandFailedWithCode(
    sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "commit-txn-2"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        stmtId: NumberInt(stmtId++),
        autocommit: false,
    }),
    ErrorCodes.ConflictingOperationInProgress,
);

session.endSession();
