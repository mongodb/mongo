/**
 * Test to ensure that rollback waits for in-progress background index builds to finish before
 * starting the rollback process. Only applies to Recoverable Rollback via WiredTiger checkpoints.
 *
 * @tags: [
 *     requires_fcv_44,
 *     requires_wiredtiger,
 *     requires_journaling,
 *     requires_majority_read_concern,
       requires_fcv_44,
 * ]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');
load("jstests/replsets/rslib.js");
load('jstests/replsets/libs/rollback_test.js');

let bgIndexThread;

const dbName = "dbWithBgIndex";
const collName = "coll";

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

    // Insert document into collection to avoid optimization for index creation on an empty
    // collection. This allows us to pause index builds on the collection using a fail point.
    const testColl = testDB.getCollection(collName);
    assert.commandWorked(testColl.insert({a: 1}));

    // Puase all index builds.
    IndexBuildTest.pauseIndexBuilds(node);

    jsTestLog("Starting background index build in parallel shell.");
    bgIndexThread = IndexBuildTest.startIndexBuild(node, testColl.getFullName(), {x: 1});

    jsTestLog("Waiting for background index build to start.");
    IndexBuildTest.waitForIndexBuildToStart(testDB, collName, "x_1");
}

const replTest = new ReplSetTest({
    name: jsTestName(),
    nodes: 3,
    useBridge: true,
});
replTest.startSet();
let config = replTest.getReplSetConfig();
config.members[2].priority = 0;
config.settings = {
    chainingAllowed: false
};
replTest.initiateWithHighElectionTimeout(config);

const rollbackTest = new RollbackTest(jsTestName(), replTest);
const originalPrimary = rollbackTest.getPrimary();
const testDB = originalPrimary.getDB(dbName);
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
checkLog.contains(rollbackNode, msg1);

jsTestLog("Resuming index builds on the original primary.");
reconnect(originalPrimary);
IndexBuildTest.resumeIndexBuilds(originalPrimary);

// Make sure the background index build completed before rollback started.
checkLog.contains(rollbackNode,
                  "Finished waiting for background operations to complete before rollback");

// Wait for rollback to finish.
rollbackTest.transitionToSteadyStateOperations();

// Ensure index build is not running.
IndexBuildTest.waitForIndexBuildToStop(testDB, collName, "x_1");

// Check the replica set.
rollbackTest.stop();
}());
