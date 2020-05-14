/**
 * This is a basic test that checks that, when election handoff is enabled, a primary that is sent a
 * non-terminal signal sends a ReplSetStepUp request to an eligible candidate.
 */

(function() {
"use strict";
load("jstests/replsets/libs/election_handoff.js");

const testName = "election_handoff_via_signal";
const numNodes = 3;
// Initiate with a higher 5 second shutdownTimeout instead of the default 100 ms to allow enough
// time for nodes to grab the RSTL while stepping down during shutdown.
const rst = ReplSetTest({
    name: testName,
    nodes: numNodes,
    nodeOptions: {setParameter: "shutdownTimeoutMillisForSignaledShutdown=5000"}
});
const nodes = rst.nodeList();
rst.startSet();

// Make sure there are no election timeouts firing for the duration of the test. This helps
// ensure that the test will only pass if the election handoff succeeds.
rst.initiateWithHighElectionTimeout();

ElectionHandoffTest.testElectionHandoff(rst, 0, 1, {stepDownBySignal: true});

rst.stopSet();
})();
