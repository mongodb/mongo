/**
 * Test that initial sync fails if a rollback occurs while querying collection data.
 *
 * This test depends on 4.4 features.
 * @tags: [multiversion_incompatible]
 */

(function() {
"use strict";

load("jstests/replsets/libs/rollback_test.js");

const testName = "initial_sync_fails_on_rollback";
const dbName = testName;

// Operations that will be present on both nodes, before the common point.
let CommonOps = (node) => {
    let testDb = node.getDB(dbName);
    let bulk = testDb.test.initializeOrderedBulkOp();
    for (let i = 0; i < 100; i++) {
        bulk.insert({a: i});
    }
    assert.commandWorked(bulk.execute({w: "majority"}));
};

// Operations that will be performed on the rollback node past the common point.
let RollbackOps = (node) => {
    let testDb = node.getDB(dbName);
    assert.commandWorked(testDb.test.insert({b: 1}));
};

// Set up Rollback Test.
let rollbackTest = new RollbackTest(testName);
CommonOps(rollbackTest.getPrimary());

// Add the initial sync node.
const initialSyncNode = rollbackTest.add({
    config: {
        // This node must be non-voting to avoid interfering with the rollback machinery.
        rsConfig: {priority: 0, votes: 0},
        setParameter: {
            'failpoint.initialSyncHangBeforeCopyingDatabases': tojson({mode: 'alwaysOn'}),
            'collectionClonerBatchSize': 10,
            // This test is specifically testing that the cloners stop, so we turn off the
            // oplog fetcher to ensure that we don't inadvertently test that instead.
            'failpoint.hangBeforeStartingOplogFetcher': tojson({mode: 'alwaysOn'}),
            // Make sure our initial sync node only talks to the rolling-back primary.
            'failpoint.forceSyncSourceCandidate':
                tojson({mode: 'alwaysOn', data: {"hostAndPort": rollbackTest.getPrimary().host}}),
            'numInitialSyncAttempts': 1,
        }
    }
});

jsTestLog("Waiting for initial sync node to reach initial sync state");
const rst = rollbackTest.getTestFixture();
rst.waitForState(initialSyncNode, ReplSetTest.State.STARTUP_2);

const afterBatchResponseFailPoint =
    configureFailPoint(initialSyncNode,
                       "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
                       {nss: dbName + ".test"});
const beforeFinishFailPoint = configureFailPoint(initialSyncNode, "initialSyncHangBeforeFinish");
// Release the initial failpoint.
assert.commandWorked(initialSyncNode.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeCopyingDatabases", mode: "off"}));

jsTestLog("Waiting for CollectionCloner to reach " + dbName + ".test collection");
afterBatchResponseFailPoint.wait();

jsTestLog("Rolling back");
let rollbackNode = rollbackTest.transitionToRollbackOperations();
RollbackOps(rollbackNode);

// Wait for rollback to finish.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
// We must skip consistency checks because our initial sync node is not consistent.
rollbackTest.transitionToSteadyStateOperations({skipDataConsistencyChecks: true});

jsTestLog("Releasing the CollectionCloner failpoint.");
afterBatchResponseFailPoint.off();

jsTestLog("Releasing the oplog fetcher failpoint.");
assert.commandWorked(initialSyncNode.getDB("test").adminCommand(
    {configureFailPoint: "hangBeforeStartingOplogFetcher", mode: "off"}));

beforeFinishFailPoint.wait();
const res = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));
// The initial sync should have failed.
assert.eq(res.initialSyncStatus.failedInitialSyncAttempts, 1);
// It should have determined the correct number of documents to copy.
assert.eq(res.initialSyncStatus.databases[dbName][dbName + ".test"].documentsToCopy, 100);
// No more than one batch should have been copied.
assert.lte(res.initialSyncStatus.databases[dbName][dbName + ".test"].documentsCopied, 10);

// Get rid of the failed node so the fixture can stop properly.
rst.stop(initialSyncNode);
rst.remove(initialSyncNode);

rollbackTest.stop();
})();
