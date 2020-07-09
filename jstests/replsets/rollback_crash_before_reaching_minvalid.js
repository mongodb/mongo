/**
 * Test that a node does not take a stable checkpoint at a timestamp earlier than minValid after
 * crashing post rollbackViaRefetch. This test exercises that behavior when run with
 * enableMajorityReadConcern:false.
 *
 * @tags: [requires_persistence]
 */
(function() {
"use strict";

load("jstests/replsets/libs/rollback_test.js");
load("jstests/libs/fail_point_util.js");

TestData.rollbackShutdowns = true;
let dbName = "test";
let sourceCollName = "coll";

let doc1 = {_id: 1, x: "document_of_interest"};

let CommonOps = (node) => {
    // Insert a document that will exist on all nodes.
    assert.commandWorked(node.getDB(dbName)[sourceCollName].insert(doc1));
};

let SyncSourceOps = (node) => {
    // Insert some documents on the sync source so the rollback node will have a minValid it needs
    // to catch up to.
    assert.commandWorked(node.getDB(dbName)[sourceCollName].insert({x: 1, sync_source: 1}));
    assert.commandWorked(node.getDB(dbName)[sourceCollName].insert({x: 2, sync_source: 1}));
    assert.commandWorked(node.getDB(dbName)[sourceCollName].insert({x: 3, sync_source: 1}));
};

let RollbackOps = (node) => {
    // Delete the document on the rollback node so it will be refetched from sync source.
    assert.commandWorked(node.getDB(dbName)[sourceCollName].remove(doc1));
};

const replTest = new ReplSetTest({nodes: 3, useBridge: true});
replTest.startSet();
// Speed up the test.
replTest.nodes.forEach(node => {
    assert.commandWorked(
        node.adminCommand({configureFailPoint: 'setSmallOplogGetMoreMaxTimeMS', mode: 'alwaysOn'}));
});
let config = replTest.getReplSetConfig();
config.members[2].priority = 0;
config.settings = {
    chainingAllowed: false
};
replTest.initiateWithHighElectionTimeout(config);
let rollbackTest = new RollbackTest("rollback_crash_before_reaching_minvalid", replTest);
CommonOps(rollbackTest.getPrimary());

let rollbackNode = rollbackTest.transitionToRollbackOperations();

// Have the node hang after rollback has completed but before it starts applying ops again.
rollbackNode.adminCommand({configureFailPoint: 'bgSyncHangAfterRunRollback', mode: 'alwaysOn'});
RollbackOps(rollbackNode);

let node = rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
SyncSourceOps(node);

// Let the rollback run.
rollbackTest.transitionToSyncSourceOperationsDuringRollback();

jsTestLog("Waiting for the rollback node to hit the failpoint.");
checkLog.contains(rollbackNode, "bgSyncHangAfterRunRollback failpoint is set");

// Kill the rollback node before it has reached minValid. Sending a shutdown signal to the node
// should cause us to break out of the hung failpoint, so we don't need to explicitly turn the
// failpoint off.
jsTestLog("Killing the rollback node.");
replTest.stop(0, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});
replTest.start(
    0,
    {
        setParameter: {
            // Pause oplog fetching so the node doesn't advance past minValid after restart.
            "failpoint.stopReplProducer": "{'mode':'alwaysOn'}"
        }
    },
    true /* restart */);

// Wait long enough for the initial stable checkpoint to be triggered if it was going to be. We
// expect that no stable checkpoints are taken. If they are, we expect the test to fail when we
// restart below and recover from a stable checkpoint.
//
// First we wait until the node has a commit point, since learning of one should trigger an update
// to the stable timestamp. Then, we wait for a bit after this for any potential checkpoint to
// occur. In the worst case, if the checkpoint was very slow to complete, we might produce a false
// negative test result (the test would pass even though a bug existed), but we consider this
// acceptable if it happens rarely.
assert.soonNoExcept(() => {
    let status = replTest.nodes[0].adminCommand({replSetGetStatus: 1});
    return status.optimes.lastCommittedOpTime.ts !== Timestamp(0, 0);
});
sleep(5000);

// Kill and restart the node to test that we don't recover from an inconsistent stable checkpoint
// taken above.
replTest.stop(0, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});
replTest.start(
    0,
    {
        setParameter: {
            // Make sure this failpoint is not still enabled in the saved startup options.
            "failpoint.stopReplProducer": "{'mode':'off'}"
        }
    },
    true /* restart */);

rollbackTest.transitionToSteadyStateOperations();

// Check the replica set.
rollbackTest.stop();
}());