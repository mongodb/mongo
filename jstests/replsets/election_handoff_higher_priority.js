/**
 * This is a test that checks that, with election handoff is enabled, a primary that steps
 * down sends a ReplSetStepUp request to an eligible candidate. This test uses a three node
 * replica set, where one of the secondaries has a higher priority than the other. The test
 * expects that that secondary gets chosen as the election handoff candidate.
 */

(function() {
"use strict";
load("jstests/replsets/libs/election_handoff.js");

const testName = "election_handoff_higher_priority";
const numNodes = 3;
const rst = ReplSetTest({name: testName, nodes: numNodes});
const nodes = rst.nodeList();
rst.startSet();

const config = rst.getReplSetConfig();
config.members[0].priority = 3;
config.members[1].priority = 1;
config.members[2].priority = 2;

// Make sure there are no election timeouts firing for the duration of the test. This helps
// ensure that the test will only pass if the election handoff succeeds.
config.settings = {
    "electionTimeoutMillis": 12 * 60 * 60 * 1000
};
rst.initiate(config);

ElectionHandoffTest.testElectionHandoff(rst, 0, 2);

rst.stopSet();
})();
