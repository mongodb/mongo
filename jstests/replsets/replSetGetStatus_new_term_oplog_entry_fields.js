/**
 * Tests that the 'newTermStartDate' and 'wMajorityWriteAvailabilityDate' fields of the
 * replSetGetStatus 'electionCandidateMetrics' section are present only when they should be.
 */

(function() {
    "use strict";
    load("jstests/libs/write_concern_util.js");
    load("jstests/replsets/rslib.js");

    const name = jsTestName();
    const rst = new ReplSetTest({name: name, nodes: 3});

    rst.startSet();
    rst.initiateWithHighElectionTimeout();
    rst.awaitReplication();

    stopServerReplication(rst.nodes);

    // Step up one of the secondaries.
    const newPrimary = rst.getSecondary();
    assert.soonNoExcept(function() {
        assert.commandWorked(newPrimary.adminCommand({replSetStepUp: 1}));
        rst.awaitNodesAgreeOnPrimary(rst.kDefaultTimeoutMS, rst.nodes, rst.getNodeId(newPrimary));
        return newPrimary.adminCommand('replSetGetStatus').myState === ReplSetTest.State.PRIMARY;
    }, 'failed to step up node ' + newPrimary.host, rst.kDefaultTimeoutMS);

    // Wait until the new primary completes the transition to primary and writes a no-op.
    assert.eq(rst.getPrimary(), newPrimary);

    // Check that the 'electionCandidateMetrics' section of the replSetGetStatus response has the
    // 'newTermStartDate' field once the transition to primary is complete.
    let res = assert.commandWorked(newPrimary.adminCommand({replSetGetStatus: 1}));
    assert(res.electionCandidateMetrics,
           () => "Response should have an 'electionCandidateMetrics' field: " + tojson(res));
    assert(res.electionCandidateMetrics.newTermStartDate,
           () => "Response should have an 'electionCandidateMetrics.newTermStartDate' field: " +
               tojson(res.electionCandidateMetrics));

    // Check that the 'electionCandidateMetrics' section of the replSetGetStatus response does not
    // have
    // the 'wMajorityWriteAvailabilityDate' field before the new term oplog entry has been
    // replicated.
    assert(
        !res.electionCandidateMetrics.wMajorityWriteAvailabilityDate,
        () =>
            "Response should not have an 'electionCandidateMetrics.wMajorityWriteAvailabilityDate' field: " +
            tojson(res.electionCandidateMetrics));

    restartReplSetReplication(rst);
    rst.awaitReplication();

    // Check that the 'electionCandidateMetrics' section of the replSetGetStatus response has the
    // 'wMajorityWriteAvailabilityDate' field once the new term oplog entry is in the committed
    // snapshot.
    res = assert.commandWorked(newPrimary.adminCommand({replSetGetStatus: 1}));
    assert(res.electionCandidateMetrics,
           () => "Response should have an 'electionCandidateMetrics' field: " + tojson(res));
    assert(
        res.electionCandidateMetrics.wMajorityWriteAvailabilityDate,
        () =>
            "Response should have an 'electionCandidateMetrics.wMajorityWriteAvailabilityDate' field: " +
            tojson(res.electionCandidateMetrics));

    rst.stopSet();
})();
