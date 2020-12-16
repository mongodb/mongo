/**
 * Tests session invalidation and checking out a session without refresh on a new secondary.
 *
 * Tests this by:
 * 1. Starting with a primary that is running a transaction. We will hang the primary before it
 *    checks out the session for the transaction.
 * 2. Step up another node and prepare a transaction on the same session used for the transaction on
 *    the old primary. This should cause the old primary to step down, invalidating the relevant
 *    session.
 * 3. When the old primary replicates the prepared transaction, wait so that the update to the
 *    config.transactions table for the prepared transaction happens before the node prepares the
 *    transaction. Even though the session is still invalidated, applying the prepare should check
 *    out the session without refreshing from disk.
 *
 * See SERVER-50486 for more details.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/fail_point_util.js");

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();

const dbName = "test";
const collName = "coll";
const primary = replTest.getPrimary();
const newPrimary = replTest.getSecondary();

const testDB = primary.getDB(dbName);
testDB.dropDatabase();
assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

const session = primary.startSession({causalConsistency: false});
const sessionID = session.getSessionId();

let failPoint = configureFailPoint(primary, "hangBeforeSessionCheckOut");

const txnFunc = function(sessionID) {
    load("jstests/core/txns/libs/prepare_helpers.js");
    const session = PrepareHelpers.createSessionWithGivenId(db.getMongo(), sessionID);
    const sessionDB = session.getDatabase("test");
    session.startTransaction({writeConcern: {w: "majority"}});
    assert.commandFailedWithCode(
        sessionDB.runCommand({find: "test", readConcern: {level: "snapshot"}}),
        ErrorCodes.InterruptedDueToReplStateChange);
};
const waitForTxnShell = startParallelShell(funWithArgs(txnFunc, sessionID), primary.port);
failPoint.wait();

replTest.stepUp(newPrimary);
assert.eq(replTest.getPrimary(), newPrimary, "Primary didn't change.");

const prepareTxnFunc = function(sessionID) {
    load("jstests/core/txns/libs/prepare_helpers.js");
    const newPrimaryDB = db.getMongo().getDB("test");

    // Start a transaction on the same session as before, but with a higher transaction number.
    assert.commandWorked(newPrimaryDB.runCommand({
        insert: "coll",
        documents: [{c: 1}],
        lsid: sessionID,
        txnNumber: NumberLong(10),
        startTransaction: true,
        autocommit: false
    }));
    assert.commandWorked(newPrimaryDB.adminCommand({
        prepareTransaction: 1,
        lsid: sessionID,
        txnNumber: NumberLong(10),
        autocommit: false,
        writeConcern: {w: "majority"}
    }));
};

let applyFailPoint = configureFailPoint(primary, "hangBeforeSessionCheckOutForApplyPrepare");
const waitForPrepareTxnShell =
    startParallelShell(funWithArgs(prepareTxnFunc, sessionID), newPrimary.port);
applyFailPoint.wait();

// Wait so that the update to the config.transactions table from the newly prepared transaction
// happens before the user transaction checks out the session. Otherwise, we won't see the
// transaction state as being "Prepared" when refreshing the session from storage.
sleep(10000);

failPoint.off();

// Wait so that the user transaction checks out the session before the thread applying the
// prepareTransaction is unpaused. Otherwise, applying the prepareTransaction will make the session
// valid.
sleep(10000);

applyFailPoint.off();

waitForPrepareTxnShell();
waitForTxnShell();

let newPrimaryDB = replTest.getPrimary().getDB("test");
const commitTimestamp =
    assert.commandWorked(newPrimaryDB.runCommand({insert: collName, documents: [{}]})).opTime.ts;

assert.commandWorked(newPrimaryDB.adminCommand({
    commitTransaction: 1,
    commitTimestamp: commitTimestamp,
    lsid: sessionID,
    txnNumber: NumberLong(10),
    autocommit: false
}));

replTest.stopSet();
})();
