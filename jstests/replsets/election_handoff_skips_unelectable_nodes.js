/*
 * Tests that election handoff will not attempt to step up a node that is unelectable.
 */

(function() {
"use strict";

load("jstests/replsets/libs/election_handoff.js");

const rst = new ReplSetTest({name: jsTestName(), nodes: 3});
rst.startSet();

// Make sure there are no election timeouts firing for the duration of the test. This helps
// ensure that the test will only pass if the election handoff succeeds.
rst.initiateWithHighElectionTimeout();

// Freeze one of the secondaries so that it cannot be elected. As a result, the other secondary
// should always be chosen to run for primary via election handoff.
const frozenSecondary = rst.getSecondaries()[0];
assert.commandWorked(frozenSecondary.adminCommand({replSetFreeze: ReplSetTest.kForeverSecs}));

ElectionHandoffTest.testElectionHandoff(rst, 0, 2);

rst.stopSet();
})();
