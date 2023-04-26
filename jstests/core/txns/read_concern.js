// Test that no matter the readConcern specified on a multi-statement transaction, it has snapshot
// isolation. Also test that readConcern linearizable and available are not allowed with a
// transaction.
// The test runs commands that are not allowed with security token: endSession.
// @tags: [
//   not_allowed_with_security_token,uses_transactions, requires_majority_read_concern]
(function() {
"use strict";

const dbName = "test";
const collName = "read_concern";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB[collName];

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

assert.commandWorked(testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));

const sessionOptions = {
    causalConsistency: false
};
const session = db.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);

function testReadConcernMaintainsSnapshotIsolationInTransaction(readConcern) {
    jsTest.log(
        "Test that the following multi-document transaction has snapshot isolation with readConcern: " +
        tojson(readConcern));
    assert.commandWorked(testColl.remove({}, {writeConcern: {w: "majority"}}));

    // Start a new transaction with the given readConcern.
    session.startTransaction();
    let command = {find: collName};
    if (readConcern) {
        Object.extend(command, {readConcern: readConcern});
    }
    assert.commandWorked(sessionDb.runCommand(command));

    // Insert a document outside of the transaction.
    assert.commandWorked(testColl.insert({_id: 0}, {writeConcern: {w: "majority"}}));

    // Test that the transaction does not see the new document (it has snapshot isolation).
    let res = assert.commandWorked(sessionDb.runCommand({find: collName}));
    assert.eq(res.cursor.firstBatch.length, 0, tojson(res));

    // Commit the transaction.
    assert.commandWorked(session.commitTransaction_forTesting());
}

testReadConcernMaintainsSnapshotIsolationInTransaction(null);
testReadConcernMaintainsSnapshotIsolationInTransaction({});
testReadConcernMaintainsSnapshotIsolationInTransaction({level: "local"});
testReadConcernMaintainsSnapshotIsolationInTransaction({level: "majority"});
testReadConcernMaintainsSnapshotIsolationInTransaction({level: "snapshot"});

function testReadConcernNotSupportedInTransaction(readConcern) {
    jsTest.log(
        "Test that the following readConcern is not supported in a multi-document transaction: " +
        readConcern);

    // Start a new transaction with the given readConcern.
    session.startTransaction();
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName, readConcern: readConcern}),
                                 ErrorCodes.InvalidOptions);

    // No more operations are allowed in the transaction.
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName}),
                                 ErrorCodes.NoSuchTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
}

testReadConcernNotSupportedInTransaction({level: "available"});
testReadConcernNotSupportedInTransaction({level: "linearizable"});

jsTest.log("Test starting a transaction with an invalid readConcern");

// Start a new transaction with the given readConcern.
session.startTransaction();
assert.commandFailedWithCode(sessionDb.runCommand({find: collName, readConcern: {level: "bad"}}),
                             ErrorCodes.FailedToParse);

// No more operations are allowed in the transaction.
assert.commandFailedWithCode(sessionDb.runCommand({find: collName}), ErrorCodes.NoSuchTransaction);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

jsTest.log("Test specifying readConcern on the second statement in a transaction");

// Start a new transaction with snapshot readConcern.
session.startTransaction();
assert.commandWorked(sessionDb.runCommand({find: collName, readConcern: {level: "snapshot"}}));

// The second statement cannot specify a readConcern.
assert.commandFailedWithCode(
    sessionDb.runCommand({find: collName, readConcern: {level: "snapshot"}}),
    ErrorCodes.InvalidOptions);

// The transaction is still active and can be committed.
assert.commandWorked(session.commitTransaction_forTesting());

jsTest.log("Test specifying non-snapshot readConcern on the second statement in a transaction");

// Start a new transaction with majority readConcern.
session.startTransaction();
assert.commandWorked(sessionDb.runCommand({find: collName, readConcern: {level: "majority"}}));

// The second statement cannot specify a readConcern.
assert.commandFailedWithCode(
    sessionDb.runCommand({find: collName, readConcern: {level: "majority"}}),
    ErrorCodes.InvalidOptions);

// The transaction is still active and can be committed.
assert.commandWorked(session.commitTransaction_forTesting());

session.endSession();
}());
