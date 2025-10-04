// Makes sure all commands which are supposed to take statement ids do.  This should test all
// commands that are allowed in transactions.
//
// @tags: [
//    # The test runs commands that are not allowed with security token: endSession,
//    # prepareTransaction.
//    not_allowed_with_signed_security_token,
//    uses_transactions,
//    uses_prepare_transaction,
// ]

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const dbName = "test";
const collName = "statement_ids_accepted";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB[collName];

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

assert.commandWorked(testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));

const sessionOptions = {
    causalConsistency: false,
};
const session = db.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);
let txnNumber = 0;

jsTestLog("Check that abortTransaction accepts a statement ID");
assert.commandWorked(
    sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);
// abortTransaction can only be run on the admin database.
assert.commandWorked(
    sessionDb.adminCommand({
        abortTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        stmtId: NumberInt(1),
        autocommit: false,
    }),
);

jsTestLog("Check that aggregate accepts a statement ID");
assert.commandWorked(
    sessionDb.runCommand({
        aggregate: collName,
        cursor: {},
        pipeline: [{$match: {}}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);

// The applyOps command is intentionally left out.

jsTestLog("Check that commitTransaction accepts a statement ID");
assert.commandWorked(
    sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);
// commitTransaction can only be run on the admin database.
assert.commandWorked(
    sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        stmtId: NumberInt(1),
        autocommit: false,
    }),
);

jsTestLog("Check that delete accepts a statement ID");
assert.commandWorked(
    sessionDb.runCommand({
        delete: collName,
        deletes: [{q: {}, limit: 1}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);

jsTestLog("Check that distinct accepts a statement ID");
assert.commandWorked(
    sessionDb.runCommand({
        distinct: collName,
        key: "x",
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);

jsTestLog("Check that find and getmore accept a statement ID");
// Put in some data to find so getMore has a cursor to use.
assert.commandWorked(testColl.insert([{_id: 0}, {_id: 1}], {writeConcern: {w: "majority"}}));
let res = assert.commandWorked(
    sessionDb.runCommand({
        find: collName,
        batchSize: 1,
        filter: {},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);

assert.commandWorked(
    sessionDb.runCommand({
        getMore: res.cursor.id,
        collection: collName,
        batchSize: 1,
        txnNumber: NumberLong(txnNumber++),
        stmtId: NumberInt(1),
        autocommit: false,
    }),
);

jsTestLog("Check that findandmodify accepts a statement ID");
assert.commandWorked(
    sessionDb.runCommand({
        findandmodify: collName,
        remove: true,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);

jsTestLog("Check that findAndModify accepts a statement ID");
assert.commandWorked(
    sessionDb.runCommand({
        findAndModify: collName,
        remove: true,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);

// Abort the transaction to release locks.
// abortTransaction can only be run on the admin database.
assert.commandWorked(
    sessionDb.adminCommand({
        abortTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        stmtId: NumberInt(0),
        autocommit: false,
    }),
);

jsTestLog("Check that insert accepts a statement ID");
assert.commandWorked(
    sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "doc1"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);

// Abort the transaction to release locks.
// abortTransaction can only be run on the admin database.
assert.commandWorked(
    sessionDb.adminCommand({
        abortTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        stmtId: NumberInt(1),
        autocommit: false,
    }),
);

if (!FixtureHelpers.isMongos(db)) {
    // Skip commands that do not exist on mongos.

    jsTestLog("Check that prepareTransaction accepts a statement ID");
    assert.commandWorked(
        sessionDb.runCommand({
            insert: collName,
            documents: [{_id: "doc2"}],
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false,
        }),
    );
    // prepareTransaction can only be run on the admin database.
    assert.commandWorked(
        sessionDb.adminCommand({
            prepareTransaction: 1,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(1),
            autocommit: false,
        }),
    );
    assert.commandWorked(
        sessionDb.adminCommand({
            abortTransaction: 1,
            txnNumber: NumberLong(txnNumber++),
            stmtId: NumberInt(2),
            autocommit: false,
        }),
    );
    assert.commandFailedWithCode(
        sessionDb.runCommand({
            prepareTransaction: 1,
            txnNumber: NumberLong(txnNumber++),
            stmtId: NumberInt(0),
            autocommit: false,
        }),
        ErrorCodes.Unauthorized,
    );
}

// refreshLogicalSessionCacheNow is intentionally omitted.

jsTestLog("Check that update accepts a statement ID");
assert.commandWorked(
    sessionDb.runCommand({
        update: collName,
        updates: [{q: {_id: "doc1"}, u: {$inc: {a: 1}}}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);

// Abort the last transaction because it appears the system stalls during shutdown if
// a transaction is open.
// abortTransaction can only be run on the admin database.
assert.commandWorked(
    sessionDb.adminCommand({
        abortTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        stmtId: NumberInt(1),
        autocommit: false,
    }),
);

session.endSession();
