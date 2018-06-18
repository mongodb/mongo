/**
 * Tests prepared transaction support.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    const dbName = "test";
    const collName = "prepare_transaction";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB.getCollection(collName);

    testColl.drop();
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    const doc1 = {_id: 1, x: 1};

    // ---- Test 1. Insert a single document and run prepare. ----

    session.startTransaction();
    assert.commandWorked(sessionDB.runCommand({
        insert: collName,
        documents: [doc1],
    }));

    // Insert should not be visible outside the session.
    assert.eq(null, testColl.findOne(doc1));

    // Insert should be visible in this session.
    assert.eq(doc1, sessionColl.findOne(doc1));

    assert.commandWorked(sessionDB.adminCommand({prepareTransaction: 1}));
    session.abortTransaction();

    // After abort the insert is rolled back.
    assert.eq(null, testColl.findOne(doc1));

    // ---- Test 2. Update a document and run prepare. ----

    // Insert a document to update.
    assert.commandWorked(
        testDB.runCommand({insert: collName, documents: [doc1], writeConcern: {w: "majority"}}));

    session.startTransaction();
    assert.commandWorked(sessionDB.runCommand({
        update: collName,
        updates: [{q: doc1, u: {$inc: {x: 1}}}],
    }));

    const doc2 = {_id: 1, x: 2};

    // Update should not be visible outside the session.
    assert.eq(null, testColl.findOne(doc2));

    // Update should be visible in this session.
    assert.eq(doc2, sessionColl.findOne(doc2));

    assert.commandWorked(sessionDB.adminCommand({prepareTransaction: 1}));
    session.abortTransaction();

    // After abort the update is rolled back.
    assert.eq(doc1, testColl.findOne({_id: 1}));

    // ---- Test 3. Delete a document and run prepare. ----

    // Update the document.
    assert.commandWorked(testDB.runCommand({
        update: collName,
        updates: [{q: doc1, u: {$inc: {x: 1}}}],
        writeConcern: {w: "majority"}
    }));

    session.startTransaction();
    assert.commandWorked(sessionDB.runCommand({
        delete: collName,
        deletes: [{q: doc2, limit: 1}],
    }));

    // Delete should not be visible outside the session, so the document should be.
    assert.eq(doc2, testColl.findOne(doc2));

    // Document should not be visible in this session, since the delete should be visible.
    assert.eq(null, sessionColl.findOne(doc2));

    assert.commandWorked(sessionDB.adminCommand({prepareTransaction: 1}));
    session.abortTransaction();

    // After abort the delete is rolled back.
    assert.eq(doc2, testColl.findOne(doc2));
}());
