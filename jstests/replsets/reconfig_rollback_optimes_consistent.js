/*
 * Test that a node in rollback state can safely be removed from the replica set
 * config via reconfig. See SERVER-119090.
 *
 * @tags: [
 *   requires_mongobridge,
 *   multiversion_incompatible,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {restartServerReplication} from "jstests/libs/write_concern_util.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const dbName = "test";
const collName = "rollbackCollection";

// RollbackTest begins with a 3 node replSet in steady-state replication.
let rollbackTest = new RollbackTest(jsTestName());
let rollbackNode = rollbackTest.getPrimary();
let secondTermPrimary = rollbackTest.getSecondary();
let tieBreakerNode = rollbackTest.getTieBreaker();

jsTestLog.info(
    `Isolate the current primary from the replica sets. Ops applied here will get rolled back.`,
);
rollbackTest.transitionToRollbackOperations();
assert.commandWorked(rollbackNode.getDB(dbName)[collName].insert({"num1": 123}));

jsTestLog.info(`Elect the previous secondary as the new primary.`);
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
assert.commandWorked(secondTermPrimary.getDB(dbName)[collName].insert({"num2": 123}));

jsTestLog.info(`Enable a failpoint to hang after transitioning to rollback mode.`);
const rollbackHangFailPoint = configureFailPoint(
    rollbackNode,
    "rollbackHangAfterTransitionToRollback",
);

jsTestLog.info(`Reconnect the isolated node and rollback should start.`);
rollbackTest.transitionToSyncSourceOperationsDuringRollback();

// The connection will be closed during rollback, so retries are needed here.
assert.soonNoExcept(() => {
    rollbackHangFailPoint.wait();
    return true;
}, `failed to wait for fail point ${rollbackHangFailPoint.failPointName}`);

jsTestLog.info(
    `Enable a failpoint to hang after processing heartbeat reconfig, so we can verify that the old primary was successfully removed while rolling back.`,
);
const postHbReconfigFailPoint = configureFailPoint(
    rollbackNode,
    "waitForPostActionCompleteInHbReconfig",
);

// RollbackTest stopped replication on tie breaker node, need to restart it.
// Otherwise the new config, which contains only the new primary and the tie
// breaker, cannot be majority committed.
restartServerReplication(tieBreakerNode);

// Now, we'll remove the node going through rollback from the replica set.
// The new primary will then propagate the new config to the other nodes via heartbeat.
// When the node in rollback processes a heartbeat from the primary with a new config,
// it will learn it is removed and transition to the RS_REMOVED state.
let newConfig = rollbackTest.getTestFixture().getReplSetConfigFromNode();
const initialMembers = newConfig.members;
newConfig.members = newConfig.members.filter((node) => node.host !== rollbackNode.host);
newConfig.version++;
assert.commandWorked(secondTermPrimary.adminCommand({replSetReconfig: newConfig}));
rollbackTest.getTestFixture().waitForConfigReplication(secondTermPrimary);

assert.soonNoExcept(() => {
    postHbReconfigFailPoint.wait();
    return true;
}, `failed to wait for fail point ${postHbReconfigFailPoint.failPointName}`);

jsTestLog.info(`Verify the rollback node is removed from replica set config.`);
assert.commandFailedWithCode(
    rollbackNode.adminCommand({replSetGetStatus: 1}),
    ErrorCodes.InvalidReplicaSetConfig,
);

// Enable a failpoint to pause rollback AFTER JournalFlusher is resumed but BEFORE optimes are reset.
jsTestLog.info("Enable hangBeforeResettingOpTimesAfterRollback failpoint.");
const hangBeforeResetFP = configureFailPoint(
    rollbackNode,
    "hangBeforeResettingOpTimesAfterRollback",
);

// Release the rollback failpoints so rollback can continue.
jsTestLog.info(`Release rollback fail points to let rollback proceed.`);
postHbReconfigFailPoint.off();
rollbackHangFailPoint.off();

// Wait for rollback to hit hangBeforeResettingOpTimesAfterRollback.
// At this point:
// - The JournalFlusher has been RESUMED (via StorageControl::startStorageControls())
// - The optimes have NOT been reset yet (resetLastOpTimesFromOplog() is after this failpoint)
jsTestLog.info("Waiting for rollback to hit hangBeforeResettingOpTimesAfterRollback...");
hangBeforeResetFP.wait();
jsTestLog.info("Rollback is paused. JournalFlusher has been resumed but optimes not yet reset.");

// Now enable the JournalFlusher failpoint. The JournalFlusher is running and will acquire
// a token with empty opTime and wallTime if we correctly detect that a rollback is in progress.
// Without the fix in SERVER-119090, it would acquire a token with the divergent opTime.
jsTestLog.info("Enable hangAfterJournalFlusherGetToken failpoint.");
const journalFlusherAfterTokenFP = configureFailPoint(
    rollbackNode,
    "hangAfterJournalFlusherGetToken",
);

// Wait for the JournalFlusher to acquire the token and hit the failpoint.
jsTestLog.info("Waiting for JournalFlusher to hit hangAfterJournalFlusherGetToken...");
journalFlusherAfterTokenFP.wait();

// Verify that the token optime is empty (Timestamp(0, 0), t: -1) since the node is rolling back, even though it is in the removed state.
jsTestLog.info("Verifying JournalFlusher token has empty optime during rollback...");
assert.soon(() => {
    const emptyOptimeLogs = checkLog.getFilteredLogMessages(rollbackNode, 11909001, {
        tokenOptime: (val) => val.includes("Timestamp(0, 0)") && val.includes("t: -1"),
    });
    return emptyOptimeLogs.length > 0;
}, "Timed out waiting for hangAfterJournalFlusherGetToken with empty optime");

// Now release the rollback failpoint. Rollback will call resetLastOpTimesFromOplog(),
// which resets lastWrittenOpTime to the common point.
jsTestLog.info("Release hangBeforeResettingOpTimesAfterRollback - optimes will be reset.");
hangBeforeResetFP.off();

// Give rollback a moment to call resetLastOpTimesFromOplog().
assert.soon(() => {
    return checkLog.checkContainsOnce(rollbackNode, "21592"); //  "Rollback complete"
}, "Timed out waiting for resetLastOpTimesFromOplog to complete");
jsTestLog.info("Optimes have been reset to the common point.");

// Now release the JournalFlusher. It will call onDurable() with the token gotten above, which should have empty opTime and wallTime.
// Since lastWrittenOpTime has been reset to the common point (which is LESS than the divergent opTime),
// the invariant `opTimeAndWallTime.opTime <= lastWrittenOpTime` will fail if the check was not done.
// With the check implemented correctly, the opTime should be empty so the invariant is not triggered.
jsTestLog.info("Release JournalFlusher");
journalFlusherAfterTokenFP.off();

// The invariant will trigger here if the check to make sure that the node is in rollback does not account for the fact that the node may be in removed state.

jsTestLog.info(
    `Add the removed node back the replica set config, so that we can execute the last step of the RollbackTest to transition back to steady state.`,
);
newConfig = Object.assign({}, rollbackTest.getTestFixture().getReplSetConfigFromNode());
newConfig.members = initialMembers;
newConfig.version++;

assert.commandWorked(secondTermPrimary.adminCommand({replSetReconfig: newConfig}));
rollbackTest.getTestFixture().waitForConfigReplication(secondTermPrimary);

jsTestLog.info(`Verify the removed node is added back and primary sees its state as SECONDARY.`);
rollbackTest.getTestFixture().awaitSecondaryNodes(null, [rollbackNode]);

jsTestLog.info(`Transition back to steady state.`);
rollbackTest.transitionToSteadyStateOperations();

jsTestLog.info(
    `Verify that the Journal Flusher now sees the correct opTime after the completion of the rollback.`,
);

// Wait for the JournalFlusher to acquire the token and hit the failpoint.
const steadyStateJournalFlusherFP = configureFailPoint(
    rollbackNode,
    "hangAfterJournalFlusherGetToken",
);
jsTestLog.info(
    "Waiting for JournalFlusher to hit hangAfterJournalFlusherGetToken again after transition to steady state...",
);
steadyStateJournalFlusherFP.wait();

// Verify that the token optime is now valid (non-empty) since we're in steady state.
// The log will contain two entries with id 11909001: the first from rollback (empty optime),
// and the second from steady state (valid optime). We verify that there is at least one with valid optime.
jsTestLog.info(
    "Verifying JournalFlusher token has valid optime after transition to steady state...",
);
assert.soon(() => {
    // Use getFilteredLogMessages to find log entries with ID 11909001 that have a valid (non-empty) optime.
    // A valid optime will NOT have Timestamp(0, 0).
    const validOptimeLogs = checkLog.getFilteredLogMessages(rollbackNode, 11909001, {
        tokenOptime: (val) => !val.includes("Timestamp(0, 0)"),
    });
    return validOptimeLogs.length > 0;
}, "Timed out waiting for hangAfterJournalFlusherGetToken with valid optime in steady state");

steadyStateJournalFlusherFP.off();
rollbackTest.stop();
