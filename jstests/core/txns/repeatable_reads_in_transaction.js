// Test to guarantee read repeatability, meaning that while in a transaction, we should repeatedly
// read the same data even if it was modified outside of the transaction.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: endSession.
//   not_allowed_with_signed_security_token,
//   uses_transactions,
//   uses_snapshot_read_concern
// ]
// TODO (SERVER-39704): Remove the following load after SERVER-39704 is completed
import {withRetryOnTransientTxnError} from "jstests/libs/auto_retry_transaction_in_sharding.js";

const dbName = "test";
const collName = "repeatable_reads_in_transaction";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB[collName];

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

const sessionOptions = {
    causalConsistency: false,
};
const session = db.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb.getCollection(collName);

// Initialize second session variables.
const session2 = testDB.getMongo().startSession(sessionOptions);
const session2Db = session2.getDatabase(dbName);
const session2Coll = session2Db.getCollection(collName);

// Create a constant array of documents we expect to be returned during a read-only transaction.
// The value should not change since external changes should not be visible within this
// transaction.
const expectedDocs = [{_id: 0}, {_id: 1}, {_id: 2}];

withRetryOnTransientTxnError(
    () => {
        jsTest.log("Prepopulate the collection.");
        assert.commandWorked(testColl.insert(expectedDocs, {writeConcern: {w: "majority"}}));

        jsTestLog("Start a read-only transaction on the first session.");
        session.startTransaction({writeConcern: {w: "majority"}});

        assert.sameMembers(expectedDocs, sessionColl.find().toArray());

        jsTestLog("Start a transaction on the second session that modifies the same collection.");
        session2.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

        assert.commandWorked(session2Coll.insert({_id: 3}));
        assert.commandWorked(session2Coll.update({_id: 1}, {$set: {a: 1}}));
        assert.commandWorked(session2Coll.deleteOne({_id: 2}));

        jsTestLog(
            "Continue reading in the first transaction. Changes from the second transaction should not be visible.",
        );

        assert.sameMembers(expectedDocs, sessionColl.find().toArray());

        jsTestLog("Committing the second transaction.");
        assert.commandWorked(session2.commitTransaction_forTesting());

        jsTestLog("Committed changes from the second transaction should still not be visible to the first.");

        assert.sameMembers(expectedDocs, sessionColl.find().toArray());

        jsTestLog("Writes that occur outside of a transaction should not be visible to a read only transaction.");

        assert.commandWorked(testColl.insert({_id: 4}, {writeConcern: {w: "majority"}}));

        assert.sameMembers(expectedDocs, sessionColl.find().toArray());

        jsTestLog("Committing first transaction.");
        assert.commandWorked(session.commitTransaction_forTesting());
    },
    () => {
        session.abortTransaction_forTesting();
        session2.abortTransaction_forTesting();
        // Clear all documents inside collection
        testColl.remove({});
    },
);

// Make sure the correct documents exist after committing the second transaction.
assert.sameMembers([{_id: 0}, {_id: 1, a: 1}, {_id: 3}, {_id: 4}], sessionColl.find().toArray());

session.endSession();
session2.endSession();
