/**
 * Test that doing an insert on a collection with a unique index causes a prepare conflict if that
 * collection has operations from a prepared transaction on it. To make sure that the new document
 * doesn't violate the unique index, the node will have to perform reads on documents in the
 * collection. Since there are prepared operations on documents in the collection, the read should
 * cause a prepare conflict.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");

    const dbName = "test";
    const collName = "prepare_transaction_unique_index_conflict";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB.getCollection(collName);

    testColl.drop({writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const session = db.getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    assert.commandWorked(testColl.insert({_id: 1, a: 0}));

    // Ensure that the "a" field is unique.
    assert.commandWorked(testColl.createIndex({"a": 1}, {unique: true}));

    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: 2, a: 1}));
    assert.commandWorked(sessionColl.update({_id: 2}, {$unset: {a: 1}}));
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    // While trying to insert this document, the node will have to perform reads to check if it
    // violates the unique index, which should cause a prepare conflict.
    assert.commandFailedWithCode(
        testDB.runCommand({insert: collName, documents: [{_id: 3, a: 1}], maxTimeMS: 5000}),
        ErrorCodes.MaxTimeMSExpired);

    session.abortTransaction_forTesting();
})();