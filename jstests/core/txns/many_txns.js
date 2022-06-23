// SERVER-38015 Test having many interactive transactions to ensure we don't hold on to too
// many resources (like "write tickets") and don't prevent other operations from succeeding.
// @tags: [uses_transactions]
(function() {
"use strict";

const dbName = "test";
const collName = "many_txns";
const numTxns = 150;

const testDB = db.getSiblingDB(dbName);
const coll = testDB[collName];

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
assert.commandWorked(testDB.runCommand({create: coll.getName(), writeConcern: {w: "majority"}}));

const sessionOptions = {
    causalConsistency: false
};

const startTime = new Date();

// Non-transactional write to give something to find.
const initialDoc = {
    _id: "pretransaction1",
    x: 0
};
assert.commandWorked(coll.insert(initialDoc, {writeConcern: {w: "majority"}}));

// Start many transactions, each inserting two documents.
jsTest.log("Start " + numTxns + " transactions, each inserting two documents");
var sessions = [];
for (let txnNr = 0; txnNr < numTxns; ++txnNr) {
    const session = testDB.getMongo().startSession(sessionOptions);
    sessions[txnNr] = session;
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb[collName];
    let doc = seq => ({_id: "txn-" + txnNr + "-" + seq});

    session.startTransaction();

    let docs = sessionColl.find({}).toArray();
    assert.sameMembers(docs, [initialDoc]);

    // Insert a doc within the transaction.
    assert.commandWorked(sessionColl.insert(doc(1)));

    // Read in the same transaction returns the doc, but not from other txns.
    docs = sessionColl.find({_id: {$ne: initialDoc._id}}).toArray();
    assert.sameMembers(docs, [doc(1)]);

    // Insert a doc within a transaction.
    assert.commandWorked(sessionColl.insert(doc(2)));
}
const secondDoc = {
    _id: "midtransactions",
    x: 1
};
assert.commandWorked(coll.insert(secondDoc, {writeConcern: {w: "majority"}}));

// Commit all sessions.
jsTest.log("Commit all transactions.");
let numAborted = 0;
for (let txnNr = 0; txnNr < numTxns; ++txnNr) {
    // First check that a non-transactional operation conflicts and times out quickly.
    let doc = seq => ({_id: "txn-" + txnNr + "-" + seq});
    let insertCmd = {insert: collName, documents: [doc(1)], maxTimeMS: 10};
    let insertRes = testDB.runCommand(insertCmd);

    const session = sessions[txnNr];
    let commitRes = session.commitTransaction_forTesting();
    if (commitRes.code === ErrorCodes.NoSuchTransaction) {
        ++numAborted;
        continue;
    }
    assert.commandWorked(commitRes, "couldn't commit transaction " + txnNr);
    // This assertion relies on the fact a previous transaction with the same insertion
    // has started and is still pending.
    // The test assumes inserting the same record will invariably return `MaxTimeMSExpired`
    // but errors might be raised.
    // `NetworkInterfaceExceededTimeLimit` is raised in the case the process runs out of
    // resources to either create or repurpose a network connection for this operation.
    assert.commandFailedWithCode(
        insertRes,
        [ErrorCodes.MaxTimeMSExpired, ErrorCodes.NetworkInterfaceExceededTimeLimit],
        tojson({insertCmd}));

    // Read with default read concern sees the committed transaction.
    assert.eq(doc(1), coll.findOne(doc(1)));
    assert.eq(doc(2), coll.findOne(doc(2)));
    session.endSession();
}

assert.eq(initialDoc, coll.findOne(initialDoc));
assert.eq(secondDoc, coll.findOne(secondDoc));

const elapsedTime = new Date() - startTime;
jsTest.log("Test completed with " + numAborted + " aborted transactions in " + elapsedTime + " ms");

// Check whether we should expect aborts. If the parameter doesn't exist (mongos) don't check.
const getParamRes = db.adminCommand({getParameter: 1, transactionLifetimeLimitSeconds: 1});
if (getParamRes.ok && elapsedTime < getParamRes.transactionLifetimeLimitSeconds)
    assert.eq(
        numAborted, 0, "should not get aborts when transactionLifetimeLimitSeconds not exceeded");
}());
