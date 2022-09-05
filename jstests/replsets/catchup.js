// Test the catch-up behavior of new primaries.

(function() {
"use strict";

load("jstests/libs/write_concern_util.js");
load("jstests/replsets/libs/election_metrics.js");
load("jstests/replsets/rslib.js");

var name = "catch_up";
var rst = new ReplSetTest({name: name, nodes: 3, useBridge: true, waitForKeys: true});

rst.startSet();
var conf = rst.getReplSetConfig();
conf.members[2].priority = 0;
conf.settings = {
    heartbeatIntervalMillis: 500,
    electionTimeoutMillis: 10000,
    catchUpTimeoutMillis: 4 * 60 * 1000
};
rst.initiate(conf);
rst.awaitSecondaryNodes();

var primary = rst.getPrimary();
var primaryColl = primary.getDB("test").coll;

// The default WC is majority and this test can't test catchup properly if it used majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Set verbosity for replication on all nodes.
var verbosity = {
    "setParameter": 1,
    "logComponentVerbosity": {
        "replication": {"verbosity": 2},
    }
};
rst.nodes.forEach(function(node) {
    node.adminCommand(verbosity);
});

function checkOpInOplog(node, op, count) {
    node.getDB("admin").getMongo().setSecondaryOk();
    var oplog = node.getDB("local")['oplog.rs'];
    var oplogArray = oplog.find().toArray();
    assert.eq(oplog.count(op), count, "op: " + tojson(op) + ", oplog: " + tojson(oplogArray));
}

function reconfigElectionAndCatchUpTimeout(electionTimeout, catchupTimeout) {
    // Reconnect all nodes to make sure reconfig succeeds.
    rst.nodes.forEach(reconnect);
    // Wait for the config with the new term to propagate.
    rst.waitForConfigReplication(rst.getPrimary());
    // Reconfigure replica set to decrease catchup timeout.
    var newConfig = rst.getReplSetConfigFromNode();
    newConfig.version++;
    newConfig.settings.catchUpTimeoutMillis = catchupTimeout;
    newConfig.settings.electionTimeoutMillis = electionTimeout;
    reconfig(rst, newConfig, true);
    rst.awaitReplication();
    rst.awaitNodesAgreeOnPrimary();
}

rst.awaitReplication();

jsTest.log("Case 1: The primary is up-to-date after refreshing heartbeats.");
let initialNewPrimaryStatus =
    assert.commandWorked(rst.getSecondary().adminCommand({serverStatus: 1}));

// Should complete transition to primary immediately.
var newPrimary = rst.stepUp(rst.getSecondary(), {awaitReplicationBeforeStepUp: false});
rst.awaitReplication();

// Check that the 'numCatchUps' field has not been incremented in serverStatus.
let newNewPrimaryStatus = assert.commandWorked(newPrimary.adminCommand({serverStatus: 1}));
verifyServerStatusChange(
    initialNewPrimaryStatus.electionMetrics, newNewPrimaryStatus.electionMetrics, 'numCatchUps', 0);
// Check that the 'numCatchUpsAlreadyCaughtUp' field has been incremented in serverStatus, and
// that none of the other reasons for catchup concluding has been incremented.
verifyCatchUpConclusionReason(initialNewPrimaryStatus.electionMetrics,
                              newNewPrimaryStatus.electionMetrics,
                              'numCatchUpsAlreadyCaughtUp');

// Check that the 'electionCandidateMetrics' section of the replSetGetStatus response does not have
// a 'targetCatchupOpTime' field if the target opTime for catchup is not set.
let res = assert.commandWorked(newPrimary.adminCommand({replSetGetStatus: 1}));
assert(res.electionCandidateMetrics,
       () => "Response should have an 'electionCandidateMetrics' field: " + tojson(res));
assert(!res.electionCandidateMetrics.targetCatchupOpTime,
       () => "Response should not have an 'electionCandidateMetrics.targetCatchupOpTime' field: " +
           tojson(res.electionCandidateMetrics));

// Check that the 'electionCandidateMetrics' section of the replSetGetStatus response has a
// 'numCatchUpOps' field once the primary is caught up, and that it has the correct value.
assert(res.electionCandidateMetrics.numCatchUpOps,
       () => "Response should have an 'electionCandidateMetrics.numCatchUpOps' field: " +
           tojson(res.electionCandidateMetrics));
assert.eq(res.electionCandidateMetrics.numCatchUpOps, 0);

jsTest.log("Case 2: The primary needs to catch up, succeeds in time.");
initialNewPrimaryStatus =
    assert.commandWorked(rst.getSecondaries()[0].adminCommand({serverStatus: 1}));

var stepUpResults = stopReplicationAndEnforceNewPrimaryToCatchUp(rst, rst.getSecondaries()[0]);

// Check that the 'electionCandidateMetrics' section of the replSetGetStatus response does not have
// a 'newTermStartDate' field before the transition to primary is complete.
res = assert.commandWorked(stepUpResults.newPrimary.adminCommand({replSetGetStatus: 1}));
assert(res.electionCandidateMetrics,
       () => "Response should have an 'electionCandidateMetrics' field: " + tojson(res));
assert(!res.electionCandidateMetrics.newTermStartDate,
       () => "Response should not have an 'electionCandidateMetrics.newTermStartDate' field: " +
           tojson(res.electionCandidateMetrics));

// Disable fail point to allow replication.
restartServerReplication(stepUpResults.oldSecondaries);
// getPrimary() blocks until the primary finishes drain mode.
assert.eq(stepUpResults.newPrimary, rst.getPrimary());

// Wait until the new primary completes the transition to primary and writes a no-op.
checkLog.contains(stepUpResults.newPrimary, "Transition to primary complete");
// Check that the new primary's term has been updated because of the no-op.
assert.eq(getLatestOp(stepUpResults.newPrimary).t, stepUpResults.latestOpOnNewPrimary.t + 1);

// Check that the 'electionCandidateMetrics' section of the replSetGetStatus response has a
// 'newTermStartDate' field once the transition to primary is complete.
res = assert.commandWorked(stepUpResults.newPrimary.adminCommand({replSetGetStatus: 1}));
assert(res.electionCandidateMetrics,
       () => "Response should have an 'electionCandidateMetrics' field: " + tojson(res));
assert(res.electionCandidateMetrics.newTermStartDate,
       () => "Response should have an 'electionCandidateMetrics.newTermStartDate' field: " +
           tojson(res.electionCandidateMetrics));

// Check that the 'numCatchUps' field has been incremented in serverStatus.
newNewPrimaryStatus =
    assert.commandWorked(stepUpResults.newPrimary.adminCommand({serverStatus: 1}));
verifyServerStatusChange(
    initialNewPrimaryStatus.electionMetrics, newNewPrimaryStatus.electionMetrics, 'numCatchUps', 1);
// Check that the 'numCatchUpsSucceeded' field has been incremented in serverStatus, and that
// none of the other reasons for catchup concluding has been incremented.
verifyCatchUpConclusionReason(initialNewPrimaryStatus.electionMetrics,
                              newNewPrimaryStatus.electionMetrics,
                              'numCatchUpsSucceeded');

// Check that the 'electionCandidateMetrics' section of the replSetGetStatus response has the
// 'targetCatchupOpTime' field once the primary is caught up, and that it has the correct values.
assert(res.electionCandidateMetrics.targetCatchupOpTime,
       () => "Response should have an 'electionCandidateMetrics.targetCatchupOpTime' field: " +
           tojson(res.electionCandidateMetrics));
assert.eq(res.electionCandidateMetrics.targetCatchupOpTime.ts,
          stepUpResults.latestOpOnOldPrimary.ts);
assert.eq(res.electionCandidateMetrics.targetCatchupOpTime.t, stepUpResults.latestOpOnOldPrimary.t);

// Wait for all secondaries to catch up
rst.awaitReplication();
// Check the latest op on old primary is preserved on the new one.
checkOpInOplog(stepUpResults.newPrimary, stepUpResults.latestOpOnOldPrimary, 1);
rst.awaitReplication();

jsTest.log("Case 3: The primary needs to catch up, but has to change sync source to catch up.");
// Reconfig the election timeout to be longer than 1 minute so that the third node will no
// longer be denylisted by the new primary if it happened to be at the beginning of the test.
reconfigElectionAndCatchUpTimeout(3 * 60 * 1000, conf.settings.catchUpTimeoutMillis);

stepUpResults = stopReplicationAndEnforceNewPrimaryToCatchUp(rst, rst.getSecondaries()[0]);

// Disable fail point on the voter. Wait until it catches up with the old primary.
restartServerReplication(stepUpResults.voter);
assert.commandWorked(
    stepUpResults.voter.adminCommand({replSetSyncFrom: stepUpResults.oldPrimary.host}));
// Wait until the new primary knows the last applied optime on the voter, so it will keep
// catching up after the old primary is disconnected.
assert.soon(function() {
    var replSetStatus =
        assert.commandWorked(stepUpResults.newPrimary.adminCommand({replSetGetStatus: 1}));
    var voterStatus = replSetStatus.members.filter(m => m.name == stepUpResults.voter.host)[0];
    return rs.compareOpTimes(voterStatus.optime, stepUpResults.latestOpOnOldPrimary) == 0;
});
// Disconnect the new primary and the old one.
stepUpResults.oldPrimary.disconnect(stepUpResults.newPrimary);
// Disable the failpoint, the new primary should sync from the other secondary.
restartServerReplication(stepUpResults.newPrimary);
assert.eq(stepUpResults.newPrimary, rst.getPrimary());
checkOpInOplog(stepUpResults.newPrimary, stepUpResults.latestOpOnOldPrimary, 1);
// Restore the broken connection
stepUpResults.oldPrimary.reconnect(stepUpResults.newPrimary);
rst.awaitReplication();

jsTest.log("Case 4: The primary needs to catch up, fails due to timeout.");
initialNewPrimaryStatus =
    assert.commandWorked(rst.getSecondaries()[0].adminCommand({serverStatus: 1}));

// Reconfig to make the catchup timeout shorter.
reconfigElectionAndCatchUpTimeout(conf.settings.electionTimeoutMillis, 10 * 1000);

stepUpResults = stopReplicationAndEnforceNewPrimaryToCatchUp(rst, rst.getSecondaries()[0]);
// Wait until the new primary completes the transition to primary and writes a no-op.
checkLog.contains(stepUpResults.newPrimary, "Catchup timed out after becoming primary");
restartServerReplication(stepUpResults.newPrimary);
assert.eq(stepUpResults.newPrimary, rst.getPrimary());

// Check that the 'numCatchUpsTimedOut' field has been incremented in serverStatus, and that
// none of the other reasons for catchup concluding has been incremented.
newNewPrimaryStatus =
    assert.commandWorked(stepUpResults.newPrimary.adminCommand({serverStatus: 1}));
verifyCatchUpConclusionReason(initialNewPrimaryStatus.electionMetrics,
                              newNewPrimaryStatus.electionMetrics,
                              'numCatchUpsTimedOut');

// Wait for the no-op "new primary" after winning an election, so that we know it has
// finished transition to primary.
assert.soon(function() {
    return rs.compareOpTimes(stepUpResults.latestOpOnOldPrimary,
                             getLatestOp(stepUpResults.newPrimary)) < 0;
});
// The extra oplog entries on the old primary are not replicated to the new one.
checkOpInOplog(stepUpResults.newPrimary, stepUpResults.latestOpOnOldPrimary, 0);
restartServerReplication(stepUpResults.voter);
rst.awaitReplication();

jsTest.log("Case 5: The primary needs to catch up with no timeout, then gets aborted.");
// Reconfig to make the catchup timeout infinite.
reconfigElectionAndCatchUpTimeout(conf.settings.electionTimeoutMillis, -1);
stepUpResults = stopReplicationAndEnforceNewPrimaryToCatchUp(rst, rst.getSecondaries()[0]);

initialNewPrimaryStatus =
    assert.commandWorked(stepUpResults.newPrimary.adminCommand({serverStatus: 1}));

// Abort catchup.
assert.commandWorked(stepUpResults.newPrimary.adminCommand({replSetAbortPrimaryCatchUp: 1}));

// Check that the 'numCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd' field has been
// incremented in serverStatus, and that none of the other reasons for catchup concluding has
// been incremented.
newNewPrimaryStatus =
    assert.commandWorked(stepUpResults.newPrimary.adminCommand({serverStatus: 1}));
verifyCatchUpConclusionReason(initialNewPrimaryStatus.electionMetrics,
                              newNewPrimaryStatus.electionMetrics,
                              'numCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd');

// Wait for the no-op "new primary" after winning an election, so that we know it has
// finished transition to primary.
assert.soon(function() {
    return rs.compareOpTimes(stepUpResults.latestOpOnOldPrimary,
                             getLatestOp(stepUpResults.newPrimary)) < 0;
});
// The extra oplog entries on the old primary are not replicated to the new one.
checkOpInOplog(stepUpResults.newPrimary, stepUpResults.latestOpOnOldPrimary, 0);
restartServerReplication(stepUpResults.oldSecondaries);
rst.awaitReplication();
checkOpInOplog(stepUpResults.newPrimary, stepUpResults.latestOpOnOldPrimary, 0);

jsTest.log("Case 6: The primary needs to catch up with no timeout, but steps down.");
initialNewPrimaryStatus =
    assert.commandWorked(rst.getSecondaries()[0].adminCommand({serverStatus: 1}));

var stepUpResults = stopReplicationAndEnforceNewPrimaryToCatchUp(rst, rst.getSecondaries()[0]);

// Step-down command should abort catchup.
assert.commandWorked(stepUpResults.newPrimary.adminCommand({replSetStepDown: 60}));

// Check that the 'numCatchUpsFailedWithError' field has been incremented in serverStatus, and
// that none of the other reasons for catchup concluding has been incremented.
newNewPrimaryStatus =
    assert.commandWorked(stepUpResults.newPrimary.adminCommand({serverStatus: 1}));
verifyCatchUpConclusionReason(initialNewPrimaryStatus.electionMetrics,
                              newNewPrimaryStatus.electionMetrics,
                              'numCatchUpsFailedWithError');

// Rename the primary.
var steppedDownPrimary = stepUpResults.newPrimary;
var newPrimary = rst.getPrimary();
assert.neq(newPrimary, steppedDownPrimary);

// Enable data replication on the stepped down primary and make sure it syncs old writes.
rst.nodes.forEach(reconnect);
restartServerReplication(stepUpResults.oldSecondaries);
rst.awaitReplication();
checkOpInOplog(steppedDownPrimary, stepUpResults.latestOpOnOldPrimary, 1);

rst.stopSet();
})();
