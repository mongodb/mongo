/**
 * Test that the stable timestamp can advance after an oplog hole is released via an abort.
 *
 * @tags: [
 *   uses_transactions,
 * ]
 */

(function() {
"use strict";
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");  // For Thread.

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();

const dbName = "test";
const collName = "stable_timestamp_coll";
const majorityWriteCollName = "stable_timestamp_coll_majority_writes";

const testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);
const majorityWriteColl = testDB.getCollection(majorityWriteCollName);
let id = 0;

// Create the necessary collections.
assert.commandWorked(testDB.createCollection(collName));
assert.commandWorked(testDB.createCollection(majorityWriteCollName));

/**
 * The general structure of each test below is as follows:
 *
 *  1. Start a command/operation C1.
 *  2. C1 reserves an oplog timestamp T1 and then pauses.
 *  3. Start a w:majority write operation C2 at T2, where T2 > T1.
 *  4. C2 completes its write and starts waiting for write concern.
 *  5. Abort operation C1.
 *  6. Ensure C2 write concern waiting completes successfully.
 *
 * The first operation can be any operation that reserves an oplog timestamp and then later aborts.
 * This could be any number of operations that write to the oplog, including DDL and/or CRUD ops. We
 * test a few different varieties below.
 *
 */

// Run a write with {w: "majority"}.
function majorityWriteFn(host, dbName, collName, doc) {
    const testDB = new Mongo(host).getDB(dbName);
    const testColl = testDB.getCollection(collName);

    assert.commandWorked(
        testColl.insert(doc, {writeConcern: {w: "majority", wtimeout: 10 * 1000}}));
}

//
// Test createCollection abort.
//

// Create a new collection.
function createCollFn(host, dbName, collName, expectedErrCode) {
    const testDB = new Mongo(host).getDB(dbName);
    jsTestLog("Creating a new collection.");
    assert.commandFailedWithCode(testDB.createCollection(collName), expectedErrCode);
}

function testCreateCollection() {
    jsTestLog("Running createCollection test.");

    // Initialize the failpoint.
    const hangCreatefailPoint =
        configureFailPoint(primary, "hangAndFailAfterCreateCollectionReservesOpTime");

    // Start operation T1.
    jsTestLog("Starting the create collection operation.");
    const createColl = new Thread(createCollFn, primary.host, dbName, "newColl", 51267);
    createColl.start();
    hangCreatefailPoint.wait();

    // Start operation T2, the majority write.
    jsTestLog("Starting the majority write operation.");
    const doc = {_id: id++};
    const majorityWrite =
        new Thread(majorityWriteFn, primary.host, dbName, majorityWriteCollName, doc);
    majorityWrite.start();

    // Wait until the majority write operation has completed and is waiting for write concern.
    assert.soon(() => majorityWriteColl.find(doc).itcount() === 1);

    jsTestLog("Releasing the failpoint.");
    hangCreatefailPoint.off();

    jsTestLog("Waiting for the operations to complete.");
    createColl.join();
    majorityWrite.join();
}

//
// Test insert abort.
//

// Insert a single document into a given collection.
function insertFn(host, dbName, collName, doc, expectedErrCode) {
    const testDB = new Mongo(host).getDB(dbName);
    const testColl = testDB.getCollection(collName);

    // Create the new collection.
    jsTestLog("Inserting document: " + tojson(doc));
    assert.commandFailedWithCode(testColl.insert(doc), expectedErrCode);
}

function testInsert() {
    jsTestLog("Running insert test.");

    const failPoint = configureFailPoint(primary,
                                         "hangAndFailAfterDocumentInsertsReserveOpTimes",
                                         {collectionNS: testColl.getFullName()});

    // Start operation T1.
    jsTestLog("Starting the insert operation.");
    const insert = new Thread(insertFn, primary.host, dbName, collName, {insert: 1}, 51269);
    insert.start();
    failPoint.wait();

    // Start operation T2, the majority write.
    jsTestLog("Starting the majority write operation.");
    const doc = {_id: id++};
    const majorityWrite =
        new Thread(majorityWriteFn, primary.host, dbName, majorityWriteCollName, doc);
    majorityWrite.start();

    // Wait until the majority write operation has completed and is waiting for write concern.
    jsTestLog("Waiting until the majority write is visible.");
    assert.soon(() => majorityWriteColl.find(doc).itcount() === 1);

    jsTestLog("Releasing the failpoint.");
    failPoint.off();

    jsTestLog("Waiting for the operations to complete.");
    insert.join();
    majorityWrite.join();
}

//
// Test unprepared transaction commit abort.
//

// Run and commit a transaction that inserts a document.
function transactionFn(host, dbName, collName) {
    const session = new Mongo(host).startSession();
    const sessionDB = session.getDatabase(dbName);

    session.startTransaction();
    sessionDB[collName].insert({});
    assert.commandFailedWithCode(session.commitTransaction_forTesting(), 51268);
}

function testUnpreparedTransactionCommit() {
    jsTestLog("Running unprepared transaction commit test.");

    const failPoint =
        configureFailPoint(primary, "hangAndFailUnpreparedCommitAfterReservingOplogSlot");

    // Start operation T1.
    jsTestLog("Starting the transaction.");
    const txn = new Thread(transactionFn, primary.host, dbName, collName);
    txn.start();
    failPoint.wait();

    // Start operation T2, the majority write.
    jsTestLog("Starting the majority write operation.");
    const doc = {_id: id++};
    const majorityWrite =
        new Thread(majorityWriteFn, primary.host, dbName, majorityWriteCollName, doc);
    majorityWrite.start();

    // Wait until the majority write operation has completed and is waiting for write concern.
    jsTestLog("Waiting until the majority write is visible.");
    assert.soon(() => majorityWriteColl.find(doc).itcount() === 1);

    jsTestLog("Releasing the failpoint.");
    failPoint.off();

    jsTestLog("Waiting for the operations to complete.");
    txn.join();
    majorityWrite.join();
}

// Execute all the tests.
testCreateCollection();
testInsert();
testUnpreparedTransactionCommit();

replTest.stopSet();
}());
