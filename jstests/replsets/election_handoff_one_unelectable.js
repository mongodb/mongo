/**
 * This is a basic test that checks that, with election handoff enabled, a primary that steps down
 * sends a ReplSetStepUp request to an eligible candidate. This test uses a three-node replica
 * set, where one of the secondaries is unelectable, so the test expects the other one to get
 * chosen for election handoff.
 */

(function() {
    "use strict";
    load("jstests/replsets/libs/election_handoff.js");

    const testName = "election_handoff_one_unelectable";
    const numNodes = 3;
    const rst = ReplSetTest({name: testName, nodes: numNodes});
    const nodes = rst.nodeList();
    rst.startSet();

    const config = rst.getReplSetConfig();
    config.members[1].priority = 0;

    // Make sure there are no election timeouts firing for the duration of the test. This helps
    // ensure that the test will only pass if the election handoff succeeds.
    config.settings = {"electionTimeoutMillis": 12 * 60 * 60 * 1000};
    rst.initiate(config);

    ElectionHandoffTest.testElectionHandoff(rst, 0, 2);

    rst.stopSet();
})();