/**
 * SERVER-37255: replSetReconfig runs on a node that is concurrently processing an election win and
 * does not result in an invariant.
 */

(function() {
"use strict";
load("jstests/libs/fail_point_util.js");
load("jstests/replsets/libs/election_handoff.js");

const rst = ReplSetTest({nodes: 2});
const nodes = rst.startSet();
const config = rst.getReplSetConfig();
// Prevent elections and set heartbeat timeout >> electionHangsBeforeUpdateMemberState.
config.settings = {
    electionTimeoutMillis: 12 * 60 * 60 * 1000,
    heartbeatTimeoutSecs: 60 * 1000
};
rst.initiate(config);

const incumbent = rst.getPrimary();
const candidate = rst.getSecondary();

jsTestLog("Step down");

const failPoint = configureFailPoint(
    candidate, "electionHangsBeforeUpdateMemberState", {waitForMillis: 10 * 1000});

// The incumbent sends replSetStepUp to the candidate for election handoff.
assert.commandWorked(incumbent.adminCommand({
    replSetStepDown: ElectionHandoffTest.stepDownPeriodSecs,
    secondaryCatchUpPeriodSecs: ElectionHandoffTest.stepDownPeriodSecs / 2
}));

jsTestLog("Wait for candidate to win the election");

failPoint.wait();

jsTestLog("Try to interrupt it with a reconfig");

config.members[nodes.indexOf(candidate)].priority = 2;
config.version++;
// While the candidate is stepping up, it it possible for the RstlKillOpThread to kill this reconfig
// command before it succeeds. Failing due to interruption on stepup or the automatic reconfig on
// stepup is acceptable here because we are testing that the reconfig command does not cause the
// server to invariant.
assert.commandWorkedOrFailedWithCode(
    candidate.adminCommand({replSetReconfig: config, force: true}),
    [ErrorCodes.InterruptedDueToReplStateChange, ErrorCodes.ConfigurationInProgress]);

failPoint.off();

rst.stopSet();
})();
