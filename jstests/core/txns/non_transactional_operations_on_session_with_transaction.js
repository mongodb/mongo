/**
 * Tests that non-transactional operations run concurrently with a transaction on the same session
 * are non-transactional, and do not see or affect any transaction state. This test avoids using
 * shell helpers to not inherit transaction state.
 *
 * @tags: [uses_transactions]
 */

(function() {
    "use strict";

    const dbName = "test";
    const collName = "non_transactional_operations_on_session_with_transactions";

    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB[collName];

    // Clean up and create test collection.
    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const sessionOptions = {causalConsistency: false};
    const session = db.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb[collName];

    let txnNumber = 0;

    /**
     * Asserts that the given result cursor has the expected contents and that it is exhausted if
     * specified.
     */
    function assertCursorBatchContents(result, expectedContents, isExhausted) {
        assert.gt(expectedContents.length, 0, "Non-empty expected contents required.");
        assert(result.hasOwnProperty("cursor"), tojson(result));
        assert(result["cursor"].hasOwnProperty("firstBatch"), tojson(result));
        assert.eq(expectedContents.length, result["cursor"]["firstBatch"].length, tojson(result));
        for (let i = 0; i < expectedContents.length; i++) {
            assert.docEq(expectedContents[i], result["cursor"]["firstBatch"][i], tojson(result));
        }
        assert.eq(isExhausted, result["cursor"]["id"] === 0, tojson(result));
    }

    const doc1 = {_id: "insert-1"};
    const doc2 = {_id: "insert-2"};

    // Insert a document in a transaction.
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [doc1],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));

    // Test that we cannot observe the insert outside of the transaction.
    assert.eq(null, testColl.findOne(doc1));
    assert.eq(null, sessionColl.findOne(doc1));
    assert.eq(null, testColl.findOne(doc2));
    assert.eq(null, sessionColl.findOne(doc2));

    // Test that we observe the insert inside of the transaction.
    assertCursorBatchContents(
        assert.commandWorked(sessionDb.runCommand(
            {find: collName, batchSize: 10, txnNumber: NumberLong(txnNumber), autocommit: false})),
        [doc1],
        false);

    // Insert a document on the session outside of the transaction.
    assert.commandWorked(sessionDb.runCommand({insert: collName, documents: [doc2]}));

    // Test that we observe the insert outside of the transaction.
    assert.eq(null, testColl.findOne(doc1));
    assert.eq(null, sessionColl.findOne(doc1));
    assert.docEq(doc2, testColl.findOne(doc2));
    assert.docEq(doc2, sessionColl.findOne(doc2));

    // Test that we do not observe the new insert inside of the transaction.
    assertCursorBatchContents(
        assert.commandWorked(sessionDb.runCommand(
            {find: collName, batchSize: 10, txnNumber: NumberLong(txnNumber), autocommit: false})),
        [doc1],
        false);

    // Commit the transaction.
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        writeConcern: {w: "majority"},
        txnNumber: NumberLong(txnNumber),
        autocommit: false
    }));

    // Test that we see both documents outside of the transaction.
    assert.docEq(doc1, testColl.findOne(doc1));
    assert.docEq(doc1, sessionColl.findOne(doc1));
    assert.docEq(doc2, testColl.findOne(doc2));
    assert.docEq(doc2, sessionColl.findOne(doc2));

}());