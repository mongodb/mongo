/**
 * Tests split prepared transaction commit on primary.
 *
 * This test does the following:
 * 1) Prepare a transaction on the old primary.
 * 2) Step up the secondary as the new primary after it applies the prepared transaction.
 * 3) Commit the (split) prepared transaction on the new primary, but block it on a failpoint part
 *    way through.
 * 4) Test that reads (under certain read concerns) would be blocked on prepare conflicts.
 * 5) Restart the new primary node and wait for it to be a primary again.
 * 6) Commit the (split) prepared transaction again.
 * And it also tests the integrity of the prepared transaction along the way.
 *
 * @tags: [
 *   requires_fcv_70,
 *   uses_transactions,
 *   uses_prepare_transaction,
 *]
 */
(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

// Verify that the documents updated in the transaction are found or not, depending on expectOld.
const checkDocuments = function(docCount, testColl, expectOld, readConcern = null) {
    for (let i = 1; i <= docCount; ++i) {
        const doc = {_id: i, x: 1};
        const expected = expectOld ? doc : {_id: i, x: 1, y: 1};

        assert.eq(expected, testColl.findOne(doc), {}, {}, readConcern);
    }
};

const replTest = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        // Set the 'syncdelay' to 1s to speed up checkpointing.
        syncdelay: 1,
        setParameter: {
            logComponentVerbosity: tojsononeline({replication: 3, command: 2}),
        }
    }
});
replTest.startSet();

// initiateWithHighElectionTimeout makes replTest.waitForPrimary() below very slow
// and adding a replTest.stepUp(primary) does not help.
replTest.initiate();

const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();

const dbName = "test";
const collName = jsTestName();
let testDB = primary.getDB(dbName);
let testColl = testDB.getCollection(collName);

// Verify that we can't insert in the transaction if it is in prepared/committed state.
// Also checks the config.transactions entry.
const checkTransaction = function(sessionDB, lsid, txnNumber, expectedState) {
    const expectedError = expectedState == "prepared" ? ErrorCodes.PreparedTransactionInProgress
                                                      : ErrorCodes.TransactionCommitted;
    assert.commandFailedWithCode(sessionDB.runCommand({
        insert: collName,
        documents: [{x: 2}],
        txnNumber: NumberLong(txnNumber),
        autocommit: false
    }),
                                 expectedError);

    const res = replTest.getPrimary()
                    .getDB("config")
                    .getCollection("transactions")
                    .find({"_id.id": lsid["id"], "txnNum": txnNumber})
                    .readConcern("majority")
                    .toArray();
    assert.eq(1, res.length);
    assert.eq(expectedState, res[0]["state"]);
};

testColl.drop({writeConcern: {w: "majority"}});
assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

let session = primary.startSession({causalConsistency: false});
const lsid = session.getSessionId();
let sessionDB = session.getDatabase(dbName);
let sessionColl = sessionDB.getCollection(collName);

jsTestLog("Inserting documents before the transaction.");

const docCount = 100;
for (let i = 1; i <= docCount; ++i) {
    assert.commandWorked(testColl.insert({_id: i, x: 1}));
}

session.startTransaction();
const txnNumber = session.getTxnNumber_forTesting();

jsTestLog("Updating documents in the transaction.");

for (let i = 1; i <= docCount; ++i) {
    assert.commandWorked(sessionColl.update({_id: i}, {$set: {y: 1}}));
}

// Updates should not be visible outside the session.
checkDocuments(docCount, testColl, true /* expectOld */);

// Updates should be visible in this session.
checkDocuments(docCount, sessionColl, false /* expectOld */);

// 1) Prepare a transaction on the old primary.
jsTestLog("Preparing the transaction.");
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

// Wait until lastStableRecoveryTimestamp >= prepareTimestamp on all nodes.
assert.soon(() => {
    const primaryLastStableRecoveryTimestamp = assert.commandWorked(
        primary.adminCommand({replSetGetStatus: 1}))["lastStableRecoveryTimestamp"];
    const secondaryLastStableRecoveryTimestamp = assert.commandWorked(
        secondary.adminCommand({replSetGetStatus: 1}))["lastStableRecoveryTimestamp"];
    jsTestLog("Awaiting last stable recovery timestamp " +
              `(primary last stable recovery: ${tojson(primaryLastStableRecoveryTimestamp)}, ` +
              `secondary last stable recovery: ${tojson(secondaryLastStableRecoveryTimestamp)}) ` +
              `prepareTimestamp: ${tojson(prepareTimestamp)}`);
    return timestampCmp(primaryLastStableRecoveryTimestamp, prepareTimestamp) >= 0 &&
        timestampCmp(secondaryLastStableRecoveryTimestamp, prepareTimestamp) >= 0;
});

checkTransaction(sessionDB, lsid, txnNumber, "prepared");

// 2) Step up the secondary as the new primary after it applies the prepared transaction.
jsTestLog("Forcing secondary to become primary.");
replTest.stepUp(secondary);
replTest.waitForState(secondary, ReplSetTest.State.PRIMARY);
const newPrimary = replTest.getPrimary();
assert.eq(newPrimary, secondary);

// Freezing the old primary so it will no longer be a primary.
jsTestLog("Freezing old primary node.");
assert.commandWorked(primary.adminCommand({replSetFreeze: ReplSetTest.kForeverSecs}));

// Force the session to use the same lsid and txnNumber as from before the stepUp.
// This ensures that we're working with the same session and transaction.
session = PrepareHelpers.createSessionWithGivenId(newPrimary, lsid);
session.setTxnNumber_forTesting(txnNumber);
sessionDB = session.getDatabase(dbName);

checkTransaction(sessionDB, lsid, txnNumber, "prepared");

// Inserts are not seen outside the transaction.
checkDocuments(docCount, testColl, true /* expectOld */);

testDB = newPrimary.getDB(dbName);
testColl = testDB.getCollection(collName);

// 3) Commit the (split) prepared transaction on the new primary, but block it on a failpoint part
//    way through.
jsTestLog("Committing transaction (with failpoint to pause split transaction commit).");

// Now the new primary will have to commit the split prepared transaction,
// so it will enter _commitSplitPreparedTxnOnPrimary.

// Set the failpoint with skip so part of the split transaction is committed and part is not.
const failPointName = "hangInCommitSplitPreparedTxnOnPrimary";
const failPoint = configureFailPoint(testDB, failPointName, {}, {skip: 2});

const commitTxnFunc = function(dbName, prepareTimestamp, lsid, txnNumber) {
    load("jstests/core/txns/libs/prepare_helpers.js");

    const session = PrepareHelpers.createSessionWithGivenId(db.getMongo(), lsid);
    session.setTxnNumber_forTesting(txnNumber);
    const sessionDB = session.getDatabase(dbName);

    const err = assert.throws(() => sessionDB.adminCommand({
        commitTransaction: 1,
        commitTimestamp: prepareTimestamp,
        txnNumber: txnNumber,
        autocommit: false,
    }));

    assert(isNetworkError(err), tojson(err));
};

const awaitCommitTransaction = startParallelShell(
    funWithArgs(commitTxnFunc, dbName, prepareTimestamp, lsid, txnNumber), newPrimary.port);

failPoint.wait();

jsTestLog("Transaction is blocked on failpoint in the middle of a split transaction commit.");

// 4) Test that reads (under certain read concerns) would be blocked on prepare conflicts.
{
    const shortTimeout = 1 * 1000;  // 1 second.
    const longTimeout = ReplSetTest.kForeverMillis;
    const read = function(readConcern, timeout) {
        return testDB.runCommand({
            find: collName,
            filter: {y: 1},
            readConcern: readConcern,
            maxTimeMS: timeout,
        });
    };

    jsTestLog("Test read with read concern 'local' doesn't block on prepared conflicts.");
    assert.commandWorked(read({level: 'local'}, longTimeout));

    jsTestLog("Test read with read concern 'majority' doesn't block on prepared conflicts.");
    assert.commandWorked(read({level: 'majority'}, longTimeout));

    jsTestLog("Test read with read concern 'linearizable' blocks on prepared conflicts.");
    assert.commandFailedWithCode(read({level: 'linearizable'}, shortTimeout),
                                 ErrorCodes.MaxTimeMSExpired);

    jsTestLog("Test afterClusterTime read after prepareTimestamp blocks on prepare conflicts.");
    assert.commandFailedWithCode(
        read({level: 'local', afterClusterTime: prepareTimestamp}, shortTimeout),
        ErrorCodes.MaxTimeMSExpired);

    jsTestLog("Test snapshot read at prepareTimestamp blocks on prepare conflicts.");
    assert.commandFailedWithCode(
        read({level: 'snapshot', atClusterTime: prepareTimestamp}, shortTimeout),
        ErrorCodes.MaxTimeMSExpired);
}

// 5) Restart the new primary node and wait for it to be a primary again.
jsTestLog("Restarting the primary node");
// Restart newPrimary.
replTest.stop(
    newPrimary, 9 /* signal */, {forRestart: true, allowedExitCode: MongoRunner.EXIT_SIGKILL});
replTest.start(newPrimary, {waitForConnect: true}, true /* waitForHealth */);
jsTestLog("Restarted the primary node");

// Join the parallel thread.
awaitCommitTransaction();

// The restarted newPrimary node should be elected as the primary again since the other node (old
// primary) was set a high freeze timeout.
replTest.waitForPrimary();
replTest.awaitSecondaryNodes();

const newPrimary2 = replTest.getPrimary();
assert.eq(newPrimary2, newPrimary);

session = PrepareHelpers.createSessionWithGivenId(newPrimary2, lsid);
session.setTxnNumber_forTesting(txnNumber);
sessionDB = session.getDatabase(dbName);

checkTransaction(sessionDB, lsid, txnNumber, "prepared");

testDB = newPrimary2.getDB(dbName);
testColl = testDB.getCollection(collName);
const secondaryTestDB = replTest.getSecondary().getDB(dbName);
const secondaryTestColl = secondaryTestDB.getCollection(collName);

// Updates are not seen outside the transaction.
checkDocuments(docCount, testColl, true /* expectOld */, "local" /* readConcern */);
checkDocuments(docCount, secondaryTestColl, true /* expectOld */, "local" /* readConcern */);

testDB = newPrimary2.getDB(dbName);
testColl = testDB.getCollection(collName);

// 6) Commit the (split) prepared transaction again.
jsTestLog("Committing transaction (this one is expected to succeed)");
assert.commandWorked(sessionDB.adminCommand({
    commitTransaction: 1,
    commitTimestamp: prepareTimestamp,
    txnNumber: txnNumber,
    autocommit: false,
}));

checkTransaction(sessionDB, lsid, txnNumber, "committed");

// After commit the updates become visible.
checkDocuments(docCount, testColl, false /* expectOld */);

replTest.stopSet();
}());
