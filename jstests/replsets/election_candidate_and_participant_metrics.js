/**
 * This test checks that the metrics around election candidates and voters are set correctly.
 */

(function() {
    "use strict";
    load("jstests/libs/check_log.js");
    load("jstests/replsets/libs/election_metrics.js");
    load("jstests/replsets/libs/election_handoff.js");

    const testName = jsTestName();
    const numNodes = 2;
    const rst = ReplSetTest({name: testName, nodes: numNodes});
    const nodes = rst.nodeList();
    rst.startSet();

    // Make sure there are no election timeouts firing for the duration of the test. This helps
    // ensure that the test will only pass if the election handoff succeeds.
    rst.initiateWithHighElectionTimeout();

    const expectedElectionTimeoutMillis = 24 * 60 * 60 * 1000;

    const originalPrimary = rst.getPrimary();
    let originalPrimaryReplSetGetStatus =
        assert.commandWorked(originalPrimary.adminCommand({replSetGetStatus: 1}));
    let originalPrimaryElectionCandidateMetrics =
        originalPrimaryReplSetGetStatus.electionCandidateMetrics;

    // Check that the 'electionCandidateMetrics' section of the replSetGetStatus response after
    // replica set startup has all of the required fields and that they are set correctly.
    assert(originalPrimaryElectionCandidateMetrics.lastElectionReason,
           () => "Response should have an 'electionCandidateMetrics.lastElectionReason' field: " +
               tojson(originalPrimaryElectionCandidateMetrics));
    assert.eq(originalPrimaryElectionCandidateMetrics.lastElectionReason, "electionTimeout");
    assert(originalPrimaryElectionCandidateMetrics.lastElectionDate,
           () => "Response should have an 'electionCandidateMetrics.lastElectionDate' field: " +
               tojson(originalPrimaryElectionCandidateMetrics));
    assert(originalPrimaryElectionCandidateMetrics.termAtElection,
           () => "Response should have an 'electionCandidateMetrics.termAtElection' field: " +
               tojson(originalPrimaryElectionCandidateMetrics));
    assert.eq(originalPrimaryElectionCandidateMetrics.termAtElection, 1);
    assert(
        originalPrimaryElectionCandidateMetrics.lastCommittedOpTimeAtElection,
        () =>
            "Response should have an 'electionCandidateMetrics.lastCommittedOpTimeAtElection' field: " +
            tojson(originalPrimaryElectionCandidateMetrics));
    assert(
        originalPrimaryElectionCandidateMetrics.lastSeenOpTimeAtElection,
        () =>
            "Response should have an 'electionCandidateMetrics.lastSeenOpTimeAtElection' field: " +
            tojson(originalPrimaryElectionCandidateMetrics));
    assert(originalPrimaryElectionCandidateMetrics.numVotesNeeded,
           () => "Response should have an 'electionCandidateMetrics.numVotesNeeded' field: " +
               tojson(originalPrimaryElectionCandidateMetrics));
    assert.eq(originalPrimaryElectionCandidateMetrics.numVotesNeeded, 1);
    assert(originalPrimaryElectionCandidateMetrics.priorityAtElection,
           () => "Response should have an 'electionCandidateMetrics.priorityAtElection' field: " +
               tojson(originalPrimaryElectionCandidateMetrics));
    assert.eq(originalPrimaryElectionCandidateMetrics.priorityAtElection, 1.0);
    assert(
        originalPrimaryElectionCandidateMetrics.electionTimeoutMillis,
        () => "Response should have an 'electionCandidateMetrics.electionTimeoutMillis' field: " +
            tojson(originalPrimaryElectionCandidateMetrics));
    // The node runs its own election before receiving the configuration, so 'electionTimeoutMillis'
    // is
    // set to the default value.
    assert.eq(originalPrimaryElectionCandidateMetrics.electionTimeoutMillis, 10000);
    assert(
        !originalPrimaryElectionCandidateMetrics.priorPrimaryMemberId,
        () =>
            "Response should not have an 'electionCandidateMetrics.priorPrimaryMemberId' field: " +
            tojson(originalPrimaryElectionCandidateMetrics));

    ElectionHandoffTest.testElectionHandoff(rst, 0, 1);

    const newPrimary = rst.getPrimary();
    let newPrimaryReplSetGetStatus =
        assert.commandWorked(newPrimary.adminCommand({replSetGetStatus: 1}));
    let newPrimaryElectionCandidateMetrics = newPrimaryReplSetGetStatus.electionCandidateMetrics;

    // Check that the 'electionCandidateMetrics' section of the replSetGetStatus response for the
    // new
    // primary has all of the required fields and that they are set correctly.
    assert(newPrimaryElectionCandidateMetrics.lastElectionReason,
           () => "Response should have an 'electionCandidateMetrics.lastElectionReason' field: " +
               tojson(newPrimaryElectionCandidateMetrics));
    assert.eq(newPrimaryElectionCandidateMetrics.lastElectionReason, "stepUpRequestSkipDryRun");
    assert(newPrimaryElectionCandidateMetrics.lastElectionDate,
           () => "Response should have an 'electionCandidateMetrics.lastElectionDate' field: " +
               tojson(newPrimaryElectionCandidateMetrics));
    assert(newPrimaryElectionCandidateMetrics.termAtElection,
           () => "Response should have an 'electionCandidateMetrics.termAtElection' field: " +
               tojson(newPrimaryElectionCandidateMetrics));
    assert.eq(newPrimaryElectionCandidateMetrics.termAtElection, 2);
    assert(
        newPrimaryElectionCandidateMetrics.lastCommittedOpTimeAtElection,
        () =>
            "Response should have an 'electionCandidateMetrics.lastCommittedOpTimeAtElection' field: " +
            tojson(newPrimaryElectionCandidateMetrics));
    assert(
        newPrimaryElectionCandidateMetrics.lastSeenOpTimeAtElection,
        () =>
            "Response should have an 'electionCandidateMetrics.lastSeenOpTimeAtElection' field: " +
            tojson(newPrimaryElectionCandidateMetrics));
    assert(newPrimaryElectionCandidateMetrics.numVotesNeeded,
           () => "Response should have an 'electionCandidateMetrics.numVotesNeeded' field: " +
               tojson(newPrimaryElectionCandidateMetrics));
    assert.eq(newPrimaryElectionCandidateMetrics.numVotesNeeded, 2);
    assert(newPrimaryElectionCandidateMetrics.priorityAtElection,
           () => "Response should have an 'electionCandidateMetrics.priorityAtElection' field: " +
               tojson(newPrimaryElectionCandidateMetrics));
    assert.eq(newPrimaryElectionCandidateMetrics.priorityAtElection, 1.0);
    assert(
        newPrimaryElectionCandidateMetrics.electionTimeoutMillis,
        () => "Response should have an 'electionCandidateMetrics.electionTimeoutMillis' field: " +
            tojson(newPrimaryElectionCandidateMetrics));
    assert.eq(newPrimaryElectionCandidateMetrics.electionTimeoutMillis,
              expectedElectionTimeoutMillis);
    // Since the previous primary's ID is 0, we directly assert that 0 is stored in the
    // priorPrimaryMemberId field.
    assert.eq(newPrimaryElectionCandidateMetrics.priorPrimaryMemberId, 0);

    // Step up the original primary.
    sleep(ElectionHandoffTest.stepDownPeriodSecs * 1000);
    ElectionHandoffTest.testElectionHandoff(rst, 1, 0);

    originalPrimaryReplSetGetStatus =
        assert.commandWorked(originalPrimary.adminCommand({replSetGetStatus: 1}));
    originalPrimaryElectionCandidateMetrics =
        originalPrimaryReplSetGetStatus.electionCandidateMetrics;

    // Check that the original primary's metrics are also being set properly after the second
    // election.
    assert.eq(originalPrimaryElectionCandidateMetrics.lastElectionReason,
              "stepUpRequestSkipDryRun");
    assert.eq(originalPrimaryElectionCandidateMetrics.termAtElection, 3);
    assert.eq(originalPrimaryElectionCandidateMetrics.numVotesNeeded, 2);
    assert.eq(originalPrimaryElectionCandidateMetrics.priorityAtElection, 1);
    assert.eq(originalPrimaryElectionCandidateMetrics.electionTimeoutMillis,
              expectedElectionTimeoutMillis);
    assert.eq(originalPrimaryElectionCandidateMetrics.priorPrimaryMemberId, 1);

    newPrimaryReplSetGetStatus =
        assert.commandWorked(newPrimary.adminCommand({replSetGetStatus: 1}));
    newPrimaryElectionCandidateMetrics = newPrimaryReplSetGetStatus.electionCandidateMetrics;

    // The other node should not have an electionCandidateMetrics, as it just stepped down.
    assert(!newPrimaryElectionCandidateMetrics,
           () => "Response should not have an 'electionCandidateMetrics' field: " +
               tojson(newPrimaryReplSetGetStatus));

    rst.stopSet();
})();