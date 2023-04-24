/**
 * Tests that we don't hit 3 way deadlock between an index builder, prepared transaction, and step
 * down.
 *
 * This tests the following scenario:
 * 1) Starts and index build.
 * 2) Prepares a transaction which holds the collection lock in IX mode.
 * 3) Waits for the index build to attempt to acquire the collection lock in X mode to abort, but
 *    blocks behind the prepared transaction due to a collection lock conflict.
 * 4) Steps down the primary, which enqueues the RSTL in X mode.
 * 5) Ensures the index build has released its RSTL lock before taking the X lock, and does not
 *    block stepDown. Since abort must acquire the RSTL to write its oplog entry, ensures that the
 *    index build is able to retry after failing once due to a stepDown.
 * 6) Steps up a new primary. Ensure that the blocked index build on the secondary does not prevent
 *    step-up from ocurring.
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */
load('jstests/noPassthrough/libs/index_build.js');
load("jstests/replsets/rslib.js");
load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/libs/fail_point_util.js");

(function() {

"use strict";

const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];

// This will cause the index build to fail with a CannotIndexParallelArrays error.
assert.commandWorked(
    primaryColl.insert({_id: 1, x: [1, 2], y: [1, 2]}, {"writeConcern": {"w": 1}}));

// Enable fail point which makes hybrid index build to hang before it aborts.
var failPoint;

const gracefulIndexBuildFeatureFlag =
    assert
        .commandWorked(
            primary.adminCommand({getParameter: 1, featureFlagIndexBuildGracefulErrorHandling: 1}))
        .featureFlagIndexBuildGracefulErrorHandling.value;
if (gracefulIndexBuildFeatureFlag) {
    // If this feature flag is enabled, index builds fail immediately instead of suppressing errors
    // until the commit phase, and always signal the primary for abort (even if it is itself). Abort
    // is only ever performed in the command thread, which is interrupted by replication state
    // transitions and retried. Abort in this case is not susceptible to the deadlock.
    // To test this, we block the index build before it starts scanning. When unblocked, it will try
    // to abort the build after the prepared transaction is holding the lock.
    failPoint = "hangAfterInitializingIndexBuild";
} else {
    // Whe the feature flag is off, the build is aborted while retrying the skipped record during
    // commit phase.
    failPoint = "hangAfterIndexBuildSecondDrain";
}
let res =
    assert.commandWorked(primary.adminCommand({configureFailPoint: failPoint, mode: "alwaysOn"}));
let timesEntered = res.count;

const indexName = 'myidx';
const indexThread = IndexBuildTest.startIndexBuild(primary,
                                                   primaryColl.getFullName(),
                                                   {x: 1, y: 1},
                                                   {name: indexName},
                                                   ErrorCodes.InterruptedDueToReplStateChange);

jsTestLog("Waiting for index build to hit failpoint");
assert.commandWorked(primary.adminCommand({
    waitForFailPoint: failPoint,
    timesEntered: timesEntered + 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

jsTestLog("Start txn");
const session = primary.startSession();
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);
session.startTransaction();
assert.commandWorked(sessionColl.insert({x: 1}, {$set: {y: 1}}));

jsTestLog("Prepare txn");
PrepareHelpers.prepareTransaction(session);

// Unblock index build, which will cause it to hang acquiring the X lock to commit.
assert.commandWorked(primary.adminCommand({configureFailPoint: failPoint, mode: "off"}));

let newPrimary = rst.getSecondary();

jsTestLog("Make primary step down");
const stepDownThread = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({"replSetStepDown": 60}));
}, primary.port);

// Wait for threads to join.
indexThread();
stepDownThread();

waitForState(primary, ReplSetTest.State.SECONDARY);
assert.neq(primary.port, newPrimary.port);

jsTestLog("Stepping-up new primary");
rst.stepUp(newPrimary);
waitForState(newPrimary, ReplSetTest.State.PRIMARY);

jsTestLog("Aborting transaction and waiting for index build to finish");
const newSession = new _DelegatingDriverSession(newPrimary, session);
assert.commandWorked(newSession.abortTransaction_forTesting());

IndexBuildTest.waitForIndexBuildToStop(newPrimary.getDB(dbName), collName, indexName);
IndexBuildTest.waitForIndexBuildToStop(primary.getDB(dbName), collName, indexName);
rst.awaitReplication();

IndexBuildTest.assertIndexes(newPrimary.getDB(dbName).getCollection(collName), 1, ["_id_"], []);
IndexBuildTest.assertIndexes(primaryColl, 1, ["_id_"], []);

rst.stopSet();
})();
