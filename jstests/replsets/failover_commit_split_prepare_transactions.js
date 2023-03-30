/**
 * Tests split prepared transaction commit on primary support.
 *
 * The test runs commands that are not allowed with security token: prepareTransaction.
 * @tags: [
 *   featureFlagApplyPreparedTxnsInParallel,uses_transactions,uses_prepare_transaction]
 */
(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

// Verify that the documents updated in the transaction are found or not, depending on expectOld.
const checkDocuments = function(docCount, testColl, expectOld, readConcern = null) {
    for (var i = 1; i <= docCount; ++i) {
        const doc = {_id: i, x: 1};
        var expected = expectOld ? doc : {_id: i, x: 1, y: 1};

        assert.eq(expected, testColl.findOne(doc), {}, {}, readConcern);
    }
};

// Verify that we can't insert in the transaction if it is in prepared/committed state.
// Also checks the config.transactions entry.
const checkTransaction = function(
    sessionDB, collName, lsid, txnNumber, transactionsColl, expectedState) {
    const expectedError = expectedState == "prepared" ? ErrorCodes.PreparedTransactionInProgress
                                                      : ErrorCodes.TransactionCommitted;
    assert.commandFailedWithCode(sessionDB.runCommand({
        insert: collName,
        documents: [{x: 2}],
        txnNumber: NumberLong(txnNumber),
        autocommit: false
    }),
                                 expectedError);

    const res = transactionsColl.find({"_id.id": lsid["id"], "txnNum": txnNumber})
                    .readConcern("majority")
                    .toArray();
    assert.eq(1, res.length);
    assert.eq(expectedState, res[0]["state"]);
};

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();

// initiateWithHighElectionTimeout makes replTest.waitForPrimary() below very slow
// and adding a replTest.stepUp(primary) does not help.
replTest.initiate();

const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();

const dbName = "test";
const collName = jsTestName();
var testDB = primary.getDB(dbName);
var testColl = testDB.getCollection(collName);

const config = primary.getDB("config");
const transactionsColl = config.getCollection("transactions");

testColl.drop({writeConcern: {w: "majority"}});
assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

var session = primary.startSession({causalConsistency: false});
const lsid = session.getSessionId();
var sessionDB = session.getDatabase(dbName);
var sessionColl = sessionDB.getCollection(collName);

jsTestLog("Inserting documents before the transaction.");

const docCount = 100;
for (var i = 1; i <= docCount; ++i) {
    assert.commandWorked(testColl.insert({_id: i, x: 1}));
}

session.startTransaction();
const txnNumber = session.getTxnNumber_forTesting();

jsTestLog("Updating documents in the transaction.");

for (var i = 1; i <= docCount; ++i) {
    assert.commandWorked(sessionColl.update({_id: i}, {$set: {y: 1}}));
}

// Updates should not be visible outside the session.
checkDocuments(docCount, testColl, true /* expectOld */);

// Updates should be visible in this session.
checkDocuments(docCount, sessionColl, false /* expectOld */);

jsTestLog("Preparing the transaction.");

let prepareTimestamp = PrepareHelpers.prepareTransaction(session);

// Wait until lastStableRecoveryTimestamp >= prepareTimestamp.
assert.soon(() => {
    var lastStableRecoveryTimestamp = replTest.status()["lastStableRecoveryTimestamp"];
    return timestampCmp(lastStableRecoveryTimestamp, prepareTimestamp) >= 0;
});

checkTransaction(sessionDB, collName, lsid, txnNumber, transactionsColl, "prepared");

jsTestLog("Forcing secondary to become primary.");

replTest.stepUp(secondary);
replTest.waitForState(secondary, ReplSetTest.State.PRIMARY);
const newPrimary = replTest.getPrimary();

// Force the session to use the same lsid and txnNumber as from before the stepUp.
// This ensures that we're working with the same session and transaction.
session = PrepareHelpers.createSessionWithGivenId(newPrimary, lsid);
session.setTxnNumber_forTesting(txnNumber);
sessionDB = session.getDatabase(dbName);

checkTransaction(sessionDB, collName, lsid, txnNumber, transactionsColl, "prepared");

// Inserts are not seen outside the transaction.
checkDocuments(docCount, testColl, true /* expectOld */);

testDB = newPrimary.getDB(dbName);
testColl = testDB.getCollection(collName);

jsTestLog("Committing transaction (with failpoint to pause split transaction commit).");

// Now the new primary will have to commit the split prepared transaction,
// so it will enter _commitSplitPreparedTxnOnPrimary.

// Set the failpoint with skip so part of the split transaction is committed and part is not.
const failPointName = "hangInCommitSplitPreparedTxnOnPrimary";
const failPoint = configureFailPoint(testDB, failPointName, {}, {skip: 2});

const awaitCommitTransaction =
    startParallelShell(funWithArgs(function(newPrimary, dbName, prepareTimestamp, lsid, txnNumber) {
        load("jstests/core/txns/libs/prepare_helpers.js");

        const conn = new Mongo(newPrimary);
        const session = PrepareHelpers.createSessionWithGivenId(conn, lsid);
        session.setTxnNumber_forTesting(txnNumber);
        const sessionDB = session.getDatabase(dbName);

        var err = assert.throws(() => sessionDB.adminCommand({
            commitTransaction: 1,
            commitTimestamp: prepareTimestamp,
            txnNumber: txnNumber,
            autocommit: false,
        }));

        assert(isNetworkError(err));
    }, newPrimary.host, dbName, prepareTimestamp, lsid, txnNumber));

failPoint.wait();

jsTestLog("Transaction is blocked on failpoint in the middle of a split transaction commit.");

// Restart newPrimary.
replTest.stop(
    newPrimary, 9 /* signal */, {forRestart: true, allowedExitCode: MongoRunner.EXIT_SIGKILL});
replTest.start(newPrimary, {waitForConnect: true}, true /* waitForHealth */);

// It has to be called at some point, or the test fails with
// "exiting due to parallel shells with unchecked return values."
awaitCommitTransaction();

replTest.waitForPrimary();
replTest.awaitSecondaryNodes();

const newPrimary2 = replTest.getPrimary();
assert.eq(newPrimary2, primary);

session = PrepareHelpers.createSessionWithGivenId(newPrimary2, lsid);
session.setTxnNumber_forTesting(txnNumber);
sessionDB = session.getDatabase(dbName);

checkTransaction(sessionDB, collName, lsid, txnNumber, transactionsColl, "prepared");

testDB = newPrimary2.getDB(dbName);
testColl = testDB.getCollection(collName);
const secondaryTestDB = replTest.getSecondary().getDB(dbName);
const secondaryTestColl = secondaryTestDB.getCollection(collName);

// Updates are not seen outside the transaction.
checkDocuments(docCount, testColl, true /* expectOld */, "local" /* readConcern */);
checkDocuments(docCount, secondaryTestColl, true /* expectOld */, "local" /* readConcern */);

testDB = newPrimary2.getDB(dbName);
testColl = testDB.getCollection(collName);

jsTestLog("Committing transaction (this one is expected to succeed)");

assert.commandWorked(sessionDB.adminCommand({
    commitTransaction: 1,
    commitTimestamp: prepareTimestamp,
    txnNumber: txnNumber,
    autocommit: false,
}));

checkTransaction(sessionDB, collName, lsid, txnNumber, transactionsColl, "committed");

// After commit the updates become visible.
checkDocuments(docCount, testColl, false /* expectOld */);

replTest.stopSet(null /*signal*/, false /*forRestart*/, {skipValidation: true});
}());
