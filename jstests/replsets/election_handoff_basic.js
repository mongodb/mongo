/**
 * This is a basic test that checks that, with election handoff is enabled, a primary that steps
 * down sends a ReplSetStepUp request to an eligible candidate. It uses a two-node replica set,
 * so there is only one secondary that can take over.
 */

(function() {
    "use strict";
    load("jstests/replsets/libs/election_handoff.js");

    const testName = "election_handoff_vanilla";
    const numNodes = 2;
    const rst = ReplSetTest({name: testName, nodes: numNodes});
    const nodes = rst.nodeList();
    rst.startSet();

    // Make sure there are no election timeouts firing for the duration of the test. This helps
    // ensure that the test will only pass if the election handoff succeeds.
    const config = rst.getReplSetConfig();
    config.settings = {"electionTimeoutMillis": 12 * 60 * 60 * 1000};
    rst.initiate(config);

    ElectionHandoffTest.testElectionHandoff(rst, 0, 1);

    rst.stopSet();
})();