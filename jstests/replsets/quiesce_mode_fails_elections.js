/**
 * Test that once a node enters quiesce mode, any concurrent or new elections cannot succeed.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");

const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: 3,
    // Override the quiesce period.
    nodeOptions: {setParameter: "shutdownTimeoutMillisForSignaledShutdown=5000"}
});

rst.startSet();
rst.initiateWithHighElectionTimeout();

const dbName = "test";
const primary = rst.getPrimary();
const secondary = rst.getSecondaries()[0];
const primaryDB = primary.getDB(dbName);

assert.commandWorked(
    primaryDB.coll.insert([{_id: 0, data: "initial data"}], {writeConcern: {w: "majority"}}));
rst.awaitReplication();

jsTestLog("Make the secondary hang before processing real election vote result.");
let voteRequestCompleteFailPoint =
    configureFailPoint(secondary, "hangBeforeOnVoteRequestCompleteCallback");

jsTestLog("Stepping up the secondary.");
const awaitStepUp = startParallelShell(() => {
    assert.commandFailedWithCode(db.adminCommand({replSetStepUp: 1}), ErrorCodes.CommandFailed);
}, secondary.port);

// Wait for secondary to hit the failpoint. Even though the election on secondary has not finished,
// the primary should step down due to seeing a higher term.
voteRequestCompleteFailPoint.wait();
rst.waitForState(primary, ReplSetTest.State.SECONDARY);

jsTestLog("Make the secondary hang after entering quiesce mode.");
let quiesceModeFailPoint = configureFailPoint(secondary, "hangDuringQuiesceMode");
rst.stop(secondary, null /*signal*/, {skipValidation: true}, {forRestart: true, waitpid: false});
quiesceModeFailPoint.wait();

jsTestLog("Unblock secondary election, the in-progress step up attempt should be cancelled");
voteRequestCompleteFailPoint.off();
awaitStepUp();
// Check log line with id 214480: "Not becoming primary, election has been cancelled".
checkLog.checkContainsOnceJson(secondary, 214480);

jsTestLog("Attempting another stepup should fail immediately due to being in quiesce mode");
assert.commandFailedWithCode(secondary.adminCommand({replSetStepUp: 1}), ErrorCodes.CommandFailed);
// Check log line with id 4615654: "Not starting an election, since we are shutting down".
checkLog.checkContainsOnceJson(secondary, 4615654);

jsTestLog("Unblock the secondary from quiesce mode");
quiesceModeFailPoint.off();

rst.stopSet();
})();
