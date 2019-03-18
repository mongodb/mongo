/**
 * SERVER-37255: replSetReconfig runs on a node that is concurrently processing an election win.
 */

(function() {
    "use strict";
    load("jstests/replsets/libs/election_handoff.js");
    load("jstests/libs/check_log.js");

    const rst = ReplSetTest({nodes: 2});
    const nodes = rst.startSet();
    const config = rst.getReplSetConfig();
    // Prevent elections and set heartbeat timeout >> electionHangsBeforeUpdateMemberState.
    config.settings = {electionTimeoutMillis: 12 * 60 * 60 * 1000, heartbeatTimeoutSecs: 60 * 1000};
    rst.initiate(config);

    const incumbent = rst.getPrimary();
    const candidate = rst.getSecondary();

    jsTestLog("Step down");

    assert.commandWorked(candidate.adminCommand({
        configureFailPoint: "electionHangsBeforeUpdateMemberState",
        mode: "alwaysOn",
        data: {waitForMillis: 10 * 1000}
    }));

    // The incumbent sends replSetStepUp to the candidate for election handoff.
    assert.adminCommandWorkedAllowingNetworkError(incumbent, {
        replSetStepDown: ElectionHandoffTest.stepDownPeriodSecs,
        secondaryCatchUpPeriodSecs: ElectionHandoffTest.stepDownPeriodSecs / 2
    });

    jsTestLog("Wait for candidate to win the election");

    checkLog.contains(
        candidate, "election succeeded - electionHangsBeforeUpdateMemberState fail point enabled");

    jsTestLog("Try to interrupt it with a reconfig");

    config.members[nodes.indexOf(candidate)].priority = 2;
    config.version++;
    assert.commandWorked(candidate.adminCommand({replSetReconfig: config, force: true}));

    assert.commandWorked(candidate.adminCommand(
        {configureFailPoint: "electionHangsBeforeUpdateMemberState", mode: "off"}));

    rst.stopSet();
})();