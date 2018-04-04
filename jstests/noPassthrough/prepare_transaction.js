/**
 * Tests prepared transaction support.
 * The current stub for prepareTransaction prepares and immediately aborts.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    let replSet = new ReplSetTest({
        name: "prepareTransaction",
        nodes: 1,
    });

    replSet.startSet();
    replSet.initiate();
    replSet.awaitSecondaryNodes();

    let collName = "prepare_txn";
    let testDB = replSet.getPrimary().getDB("test");
    let adminDB = replSet.getPrimary().getDB("admin");
    let testColl = testDB.getCollection(collName);

    testColl.drop();
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    var session = testDB.getMongo().startSession({causalConsistency: false});
    let sessionDB = session.getDatabase("test");
    let sessionColl = sessionDB.getCollection(collName);
    var txnNumber = 0;

    var doc1 = {_id: 1, x: 1};

    // Test 1. Insert a single document and run prepare.
    assert.commandWorked(sessionDB.runCommand({
        insert: collName,
        documents: [doc1],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    // Should not be visible.
    assert.eq(null, testColl.findOne(doc1));

    // Should be visible in this session.
    let res = sessionDB.runCommand(
        {find: collName, filter: doc1, txnNumber: NumberLong(txnNumber), autocommit: false});
    assert.commandWorked(res);
    assert.docEq([doc1], res.cursor.firstBatch);

    // Run prepare on the admin db, which immediately runs abort afterwards.
    assert.commandWorked(sessionDB.adminCommand(
        {prepareTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}));

    // The insert should be visible in this session, but because the prepare command immediately
    // aborts afterwards, the transaction is rolled back and the insert is not visible.
    assert.eq(null, testColl.findOne(doc1));

    res = sessionDB.runCommand({find: collName, filter: doc1});
    assert.commandWorked(res);
    assert.eq([], res.cursor.firstBatch);

    // Test 2. Update a document and run prepare.

    // Insert a document to update.
    assert.commandWorked(
        testDB.runCommand({insert: collName, documents: [doc1], writeConcern: {w: "majority"}}));

    let doc2 = {_id: 1, x: 2};
    txnNumber++;
    assert.commandWorked(sessionDB.runCommand({
        update: collName,
        updates: [{q: doc1, u: {$inc: {x: 1}}}],
        txnNumber: NumberLong(txnNumber),
        readConcern: {level: "snapshot"},
        startTransaction: true,
        autocommit: false
    }));

    // Should not be visible with default read concern.
    assert.eq(null, testColl.findOne(doc2));

    // Should be visible in this session.
    res = sessionDB.runCommand(
        {find: collName, filter: doc2, txnNumber: NumberLong(txnNumber), autocommit: false});
    assert.commandWorked(res);
    assert.docEq([doc2], res.cursor.firstBatch);

    // Run prepare, which immediately runs abort afterwards.
    assert.commandWorked(sessionDB.adminCommand(
        {prepareTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}));

    // The update should be visible in this session, but because the prepare command immediately
    // aborts afterwards, the transaction is rolled back and the update is not visible.
    res = sessionDB.runCommand({find: collName, filter: doc2});
    assert.commandWorked(res);
    assert.eq([], res.cursor.firstBatch);

    // Test 3. Delete a document and run prepare.

    // Update the document.
    assert.commandWorked(testDB.runCommand({
        update: collName,
        updates: [{q: doc1, u: {$inc: {x: 1}}}],
        writeConcern: {w: "majority"}
    }));

    txnNumber++;
    assert.commandWorked(sessionDB.runCommand({
        delete: collName,
        deletes: [{q: doc2, limit: 1}],
        txnNumber: NumberLong(txnNumber),
        readConcern: {level: "snapshot"},
        startTransaction: true,
        autocommit: false
    }));

    // Should be visible with default read concern.
    assert.eq(doc2, testColl.findOne(doc2));

    // Should not be visible in this session.
    res = sessionDB.runCommand(
        {find: collName, filter: doc2, txnNumber: NumberLong(txnNumber), autocommit: false});
    assert.commandWorked(res);
    assert.docEq([], res.cursor.firstBatch);

    // Run prepare.
    assert.commandWorked(sessionDB.adminCommand(
        {prepareTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}));

    // The delete should be visible in this session, but because the prepare command immediately
    // aborts afterwards, the transaction is rolled back and the document is still visible.
    res = sessionDB.runCommand({find: collName, filter: doc2});
    assert.commandWorked(res);
    assert.eq([doc2], res.cursor.firstBatch);
    replSet.stopSet();
}());
