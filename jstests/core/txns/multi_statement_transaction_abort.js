// Test basic multi-statement transaction abort.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: endSession.
//   not_allowed_with_signed_security_token,
//   uses_transactions,
//   uses_snapshot_read_concern
// ]

// TODO (SERVER-39704): Remove the following load after SERVER-39704 is completed
import {retryOnceOnTransientOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";

const dbName = "test";
const collName = "multi_statement_transaction_abort";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB[collName];

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));
let txnNumber = 0;

const sessionOptions = {
    causalConsistency: false,
};
const session = testDB.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);

jsTest.log("Insert two documents in a transaction and abort");

// Insert a doc within the transaction.

// TODO (SERVER-39704): We use the retryOnceOnTransientOnMongos
// function to handle how MongoS will propagate a StaleShardVersion error as a
// TransientTransactionError. After SERVER-39704 is completed the
// retryOnceOnTransientOnMongos can be removed
retryOnceOnTransientOnMongos(session, () => {
    assert.commandWorked(
        sessionDb.runCommand({
            insert: collName,
            documents: [{_id: "insert-1"}],
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(++txnNumber),
            startTransaction: true,
            autocommit: false,
        }),
    );
});

// Insert a doc within a transaction.
assert.commandWorked(
    sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "insert-2"}],
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    }),
);

// Cannot read with default read concern.
assert.eq(null, testColl.findOne({_id: "insert-1"}));
// Cannot read with default read concern.
assert.eq(null, testColl.findOne({_id: "insert-2"}));

// abortTransaction can only be run on the admin database.
assert.commandWorked(
    sessionDb.adminCommand({
        abortTransaction: 1,
        writeConcern: {w: "majority"},
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    }),
);

// Read with default read concern cannot see the aborted transaction.
assert.eq(null, testColl.findOne({_id: "insert-1"}));
assert.eq(null, testColl.findOne({_id: "insert-2"}));

jsTest.log("Insert two documents in a transaction and commit");

// Insert a doc with the same _id in a new transaction should work.
txnNumber++;
assert.commandWorked(
    sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "insert-1"}, {_id: "insert-2"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false,
    }),
);
// commitTransaction can only be called on the admin database.
assert.commandWorked(
    sessionDb.adminCommand({
        commitTransaction: 1,
        writeConcern: {w: "majority"},
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    }),
);
// Read with default read concern sees the committed transaction.
assert.eq({_id: "insert-1"}, testColl.findOne({_id: "insert-1"}));
assert.eq({_id: "insert-2"}, testColl.findOne({_id: "insert-2"}));

jsTest.log("Cannot abort empty transaction because it's not in progress");
txnNumber++;
// abortTransaction can only be called on the admin database.
let res = sessionDb.adminCommand({
    abortTransaction: 1,
    writeConcern: {w: "majority"},
    txnNumber: NumberLong(txnNumber),
    autocommit: false,
});
assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);
assert.eq(res.errorLabels, ["TransientTransactionError"]);

jsTest.log("Abort transaction on duplicated key errors");
assert.commandWorked(testColl.remove({}, {writeConcern: {w: "majority"}}));
assert.commandWorked(testColl.insert({_id: "insert-1"}, {writeConcern: {w: "majority"}}));
txnNumber++;
// The first insert works well.
assert.commandWorked(
    sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "insert-2"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false,
    }),
);
// But the second insert throws duplicated index key error.
res = assert.commandFailedWithCode(
    sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "insert-1", x: 0}],
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    }),
    ErrorCodes.DuplicateKey,
);
// DuplicateKey is not a transient error.
assert.eq(res.errorLabels, null);

// The error aborts the transaction.
// commitTransaction can only be called on the admin database.
assert.commandFailedWithCode(
    sessionDb.adminCommand({
        commitTransaction: 1,
        writeConcern: {w: "majority"},
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    }),
    ErrorCodes.NoSuchTransaction,
);
// Verify the documents are the same.
assert.eq({_id: "insert-1"}, testColl.findOne({_id: "insert-1"}));
assert.eq(null, testColl.findOne({_id: "insert-2"}));

jsTest.log("Abort transaction on write conflict errors");
assert.commandWorked(testColl.remove({}, {writeConcern: {w: "majority"}}));
txnNumber++;
const session2 = testDB.getMongo().startSession(sessionOptions);
const sessionDb2 = session2.getDatabase(dbName);
// Insert a doc from session 1.
assert.commandWorked(
    sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "insert-1", from: 1}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false,
    }),
);
let txnNumber2 = 0;
// Insert a doc from session 2 that doesn't conflict with session 1.
assert.commandWorked(
    sessionDb2.runCommand({
        insert: collName,
        documents: [{_id: "insert-2", from: 2}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber2),
        startTransaction: true,
        autocommit: false,
    }),
);
// Insert a doc from session 2 that conflicts with session 1.
res = sessionDb2.runCommand({
    insert: collName,
    documents: [{_id: "insert-1", from: 2}],
    txnNumber: NumberLong(txnNumber2),
    autocommit: false,
});
assert.commandFailedWithCode(res, ErrorCodes.WriteConflict);
assert.eq(res.errorLabels, ["TransientTransactionError"]);

// Session 1 isn't affected.
// commitTransaction can only be called on the admin database.
assert.commandWorked(
    sessionDb.adminCommand({
        commitTransaction: 1,
        writeConcern: {w: "majority"},
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    }),
);
// Transaction on session 2 is aborted.
assert.commandFailedWithCode(
    sessionDb2.adminCommand({
        commitTransaction: 1,
        writeConcern: {w: "majority"},
        txnNumber: NumberLong(txnNumber2),
        autocommit: false,
    }),
    ErrorCodes.NoSuchTransaction,
);
// Verify the documents only reflect the first transaction.
assert.eq({_id: "insert-1", from: 1}, testColl.findOne({_id: "insert-1"}));
assert.eq(null, testColl.findOne({_id: "insert-2"}));

jsTest.log("Higher transaction number aborts existing running transaction.");
txnNumber++;
assert.commandWorked(
    sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "running-txn-1"}, {_id: "running-txn-2"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false,
    }),
);
// A higher txnNumber aborts the old and inserts the same document.
txnNumber++;
assert.commandWorked(
    sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "running-txn-2"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false,
    }),
);
// commitTransaction can only be called on the admin database.
assert.commandWorked(
    sessionDb.adminCommand({
        commitTransaction: 1,
        writeConcern: {w: "majority"},
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    }),
);
// Read with default read concern sees the committed transaction but cannot see the aborted one.
assert.eq(null, testColl.findOne({_id: "running-txn-1"}));
assert.eq({_id: "running-txn-2"}, testColl.findOne({_id: "running-txn-2"}));

jsTest.log("Higher transaction number aborts existing running snapshot read.");
assert.commandWorked(testColl.remove({}, {writeConcern: {w: "majority"}}));
assert.commandWorked(testColl.insert([{doc: 1}, {doc: 2}, {doc: 3}], {writeConcern: {w: "majority"}}));
txnNumber++;
// Perform a snapshot read under a new transaction.
let runningReadResult = assert.commandWorked(
    sessionDb.runCommand({
        find: collName,
        batchSize: 2,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false,
    }),
);

// The cursor has not been exhausted.
assert(runningReadResult.hasOwnProperty("cursor"), tojson(runningReadResult));
assert.neq(0, runningReadResult.cursor.id, tojson(runningReadResult));

txnNumber++;
// Perform a second snapshot read under a new transaction.
let newReadResult = assert.commandWorked(
    sessionDb.runCommand({
        find: collName,
        // Use an explicit batchSize to avoid the config fuzzer choosing a batch size
        // that does not exhaust the cursor (which would result in a non-zero cursor ID).
        batchSize: 4,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false,
    }),
);

// The cursor has been exhausted.
assert(newReadResult.hasOwnProperty("cursor"), tojson(newReadResult));
assert.eq(0, newReadResult.cursor.id, tojson(newReadResult));
// commitTransaction can only be called on the admin database.
assert.commandWorked(
    sessionDb.adminCommand({
        commitTransaction: 1,
        writeConcern: {w: "majority"},
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    }),
);

jsTest.log("Aborting transaction attempts to wait for write concern.");
txnNumber++;
assert.commandWorked(
    sessionDb.runCommand({
        insert: collName,
        documents: [{a: 1}],
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false,
    }),
);
assert.commandFailedWithCode(
    sessionDb.adminCommand({
        abortTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        writeConcern: {w: 40},
        autocommit: false,
    }),
    ErrorCodes.UnsatisfiableWriteConcern,
);

session.endSession();
