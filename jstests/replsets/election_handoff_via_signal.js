/**
 * This is a basic test that checks that, when election handoff is enabled, a primary that is sent a
 * non-terminal signal sends a ReplSetStepUp request to an eligible candidate.
 */

(function() {
"use strict";
load("jstests/replsets/libs/election_handoff.js");

const testName = "election_handoff_via_signal";
const numNodes = 3;
const rst = ReplSetTest({name: testName, nodes: numNodes});
const nodes = rst.nodeList();
rst.startSet();

// Make sure there are no election timeouts firing for the duration of the test. This helps
// ensure that the test will only pass if the election handoff succeeds.
const config = rst.getReplSetConfig();
config.settings = {
    "electionTimeoutMillis": 12 * 60 * 60 * 1000
};
rst.initiate(config);

ElectionHandoffTest.testElectionHandoff(rst, 0, 1, {stepDownBySignal: true});

rst.stopSet();
})();
