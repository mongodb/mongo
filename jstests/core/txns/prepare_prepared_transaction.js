/**
 * Tests that we can successfully prepare a prepared transaction.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");

const dbName = "test";
const collName = "prepare_prepared_transaction";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB.getCollection(collName);

testColl.drop({writeConcern: {w: "majority"}});
assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

const session = testDB.getMongo().startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

const doc1 = {
    _id: 1,
    x: 1
};

// Attempting to prepare an already prepared transaction should return successfully with a
// prepareTimestamp.

// Client's opTime is later than the prepareOpTime, so just return the prepareTimestamp.
session.startTransaction();
assert.commandWorked(sessionColl.insert(doc1));
const firstTimestamp = PrepareHelpers.prepareTransaction(session);
const secondTimestamp = PrepareHelpers.prepareTransaction(session);
// Both prepareTimestamps should be equal.
assert.eq(firstTimestamp, secondTimestamp);
assert.commandWorked(session.abortTransaction_forTesting());

session.endSession();
}());
