/**
 * Test to ensure that rollback waits for in-progress background index builds to finish before
 * starting the rollback process. Only applies to Recoverable Rollback via WiredTiger checkpoints.
 *
 * TODO(SERVER-39451): Remove two_phase_index_builds_unsupported tag.
 * @tags: [
 *     requires_wiredtiger,
 *     requires_journaling,
 *     requires_majority_read_concern,
 *     two_phase_index_builds_unsupported,
 * ]
 */
(function() {
'use strict';

load('jstests/libs/check_log.js');
load("jstests/replsets/rslib.js");
load('jstests/replsets/libs/rollback_test.js');

const dbName = "dbWithBgIndex";
const collName = 'coll';
let bgIndexThread;

function hangIndexBuildsFailpoint(node, fpMode) {
    assert.commandWorked(node.adminCommand(
        {configureFailPoint: 'hangAfterStartingIndexBuildUnlocked', mode: fpMode}));
}

/**
 * A function to create a background index on the test collection in a parallel shell.
 */
function createBgIndexFn() {
    // Re-define constants, since they are not shared between shells.
    const dbName = "dbWithBgIndex";
    const collName = "coll";
    let testDB = db.getSiblingDB(dbName);
    jsTestLog("Starting background index build from parallel shell.");
    assert.commandWorked(testDB[collName].createIndex({x: 1}, {background: true}));
}

/**
 * Operations that will get replicated to both replica set nodes before rollback.
 *
 * These common operations are run against the node that will eventually go into rollback, so
 * the failpoints will only be enabled on the rollback node.
 */
function CommonOps(node) {
    // Create a collection on both data bearing nodes, so we can create an index on it.
    const testDB = node.getDB(dbName);
    assert.commandWorked(testDB.createCollection(collName));

    // Hang background index builds.
    hangIndexBuildsFailpoint(node, "alwaysOn");

    jsTestLog("Starting background index build parallel shell.");
    bgIndexThread = startParallelShell(createBgIndexFn, node.port);

    // Make sure the index build started and hit the failpoint.
    jsTestLog("Waiting for background index build to start and hang due to failpoint.");
    checkLog.contains(node, "index build: starting on " + testDB[collName].getFullName());
    checkLog.contains(node, "Hanging index build with no locks");
}

const rollbackTest = new RollbackTest();
const originalPrimary = rollbackTest.getPrimary();
CommonOps(originalPrimary);

// Insert a document so that there is an operation to rollback.
const rollbackNode = rollbackTest.transitionToRollbackOperations();
assert.commandWorked(rollbackNode.getDB(dbName)["rollbackColl"].insert({x: 1}));

// Allow rollback to start. There are no sync source ops.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();

// Make sure that rollback is hung waiting for the background index operation to complete.
jsTestLog("Waiting for rollback to block on the background index build completion.");
let msg1 = "Waiting for all background operations to complete before starting rollback";
let msg2 = "Waiting for 1 background operations to complete on database '" + dbName + "'";
checkLog.contains(rollbackNode, msg1);
checkLog.contains(rollbackNode, msg2);

// Now turn off the index build failpoint, allowing rollback to continue and finish.
jsTestLog(
    "Disabling 'hangAfterStartingIndexBuildUnlocked' failpoint on the rollback node so background index build can complete.");
hangIndexBuildsFailpoint(rollbackNode, "off");

// Make sure the background index build completed before rollback started.
checkLog.contains(rollbackNode,
                  "Finished waiting for background operations to complete before rollback");

// Wait for rollback to finish.
rollbackTest.transitionToSteadyStateOperations();

// Check the replica set.
rollbackTest.stop();
}());
