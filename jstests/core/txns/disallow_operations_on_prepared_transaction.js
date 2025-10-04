/**
 * Test calling various operations on a prepared transaction. Only commit, abort and prepare should
 * be allowed to be called on a prepared transaction. All other cases should fail with
 * PreparedTransactionInProgress.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: endSession, killCursors,
 *   # prepareTransaction.
 *   not_allowed_with_signed_security_token,
 *   uses_transactions,
 *   uses_prepare_transaction
 * ]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";

const dbName = "test";
const collName = "disallow_operations_on_prepared_transaction";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB.getCollection(collName);

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

const session = db.getMongo().startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

jsTestLog("Test that you can call prepareTransaction on a prepared transaction.");
session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 1}));
let firstTimestamp = PrepareHelpers.prepareTransaction(session);
let secondTimestamp = PrepareHelpers.prepareTransaction(session);
assert.eq(firstTimestamp, secondTimestamp);
assert.commandWorked(session.abortTransaction_forTesting());

jsTestLog("Test that you can call commitTransaction on a prepared transaction.");
session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 2}));
let prepareTimestamp = PrepareHelpers.prepareTransaction(session);
assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

jsTestLog("Test that you can call abortTransaction on a prepared transaction.");
session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 3}));
PrepareHelpers.prepareTransaction(session);
assert.commandWorked(session.abortTransaction_forTesting());

session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 4}));
PrepareHelpers.prepareTransaction(session);

jsTestLog("Test that you can't run an aggregation on a prepared transaction.");
assert.commandFailedWithCode(
    assert.throws(function () {
        sessionColl.aggregate({$match: {}});
    }),
    ErrorCodes.PreparedTransactionInProgress,
);

jsTestLog("Test that you can't run delete on a prepared transaction.");
let res = assert.commandFailedWithCode(sessionColl.remove({_id: 4}), ErrorCodes.PreparedTransactionInProgress);
assert.eq(res.errorLabels, ["TransientTransactionError"]);

jsTestLog("Test that you can't run distinct on a prepared transaction.");
assert.commandFailedWithCode(
    assert.throws(function () {
        sessionColl.distinct("_id");
    }),
    ErrorCodes.PreparedTransactionInProgress,
);

jsTestLog("Test that you can't run find on a prepared transaction.");
assert.commandFailedWithCode(
    assert.throws(function () {
        sessionColl.find({}).toArray();
    }),
    ErrorCodes.PreparedTransactionInProgress,
);

jsTestLog("Test that you can't run findandmodify on a prepared transaction.");
assert.commandFailedWithCode(
    sessionDB.runCommand({
        findandmodify: collName,
        remove: true,
        txnNumber: NumberLong(session.getTxnNumber_forTesting()),
        stmtId: NumberInt(1),
        autocommit: false,
    }),
    ErrorCodes.PreparedTransactionInProgress,
);

jsTestLog("Test that you can't run findAndModify on a prepared transaction.");
assert.commandFailedWithCode(
    assert.throws(function () {
        sessionColl.findAndModify({query: {_id: 4}, remove: true});
    }),
    ErrorCodes.PreparedTransactionInProgress,
);

jsTestLog("Test that you can't insert on a prepared transaction.");
res = assert.commandFailedWithCode(sessionColl.insert({_id: 5}), ErrorCodes.PreparedTransactionInProgress);
assert.eq(res.errorLabels, ["TransientTransactionError"]);

jsTestLog("Test that you can't run update on a prepared transaction.");
res = assert.commandFailedWithCode(sessionColl.update({_id: 4}, {a: 1}), ErrorCodes.PreparedTransactionInProgress);
assert.eq(res.errorLabels, ["TransientTransactionError"]);
assert.commandWorked(session.abortTransaction_forTesting());

jsTestLog("Test that you can't run getMore on a prepared transaction.");
session.startTransaction();
res = assert.commandWorked(sessionDB.runCommand({find: collName, batchSize: 1}));
assert(res.hasOwnProperty("cursor"), tojson(res));
assert(res.cursor.hasOwnProperty("id"), tojson(res));
PrepareHelpers.prepareTransaction(session);
assert.commandFailedWithCode(
    sessionDB.runCommand({getMore: res.cursor.id, collection: collName}),
    ErrorCodes.PreparedTransactionInProgress,
);

jsTestLog("Test that you can't run killCursors on a prepared transaction.");
assert.commandFailedWithCode(
    sessionDB.runCommand({killCursors: collName, cursors: [res.cursor.id]}),
    ErrorCodes.PreparedTransactionInProgress,
);
assert.commandWorked(session.abortTransaction_forTesting());

session.endSession();
