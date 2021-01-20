/**
 * Test that election handoff works correctly in the case where a node is caught up with primary's
 * lastApplied but is not immediately electable. (See SERVER-53612)
 */

(function() {
"use strict";
load("jstests/replsets/libs/election_handoff.js");

const testName = jsTestName();
const rst = ReplSetTest({name: testName, nodes: 2});
rst.startSet();

// Make sure there are no election timeouts firing for the duration of the test. This helps
// ensure that the test will only pass if the election handoff succeeds.
rst.initiateWithHighElectionTimeout();

rst.awaitLastOpCommitted();

// ElectionHandoffTest.testElectionHandoff uses a 30s secondaryCatchUpPeriodSecs, freeze the
// secondary for 15s so that the secondary is not immediately electable during election handoff.
const secondary = rst.getSecondary();
assert.commandWorked(secondary.adminCommand({replSetFreeze: 15}));
// replSetStepUp should fail due to replSetFreeze.
assert.commandFailedWithCode(secondary.adminCommand({replSetStepUp: 1}), ErrorCodes.CommandFailed);

// Test that the election handoff works eventually when the secondary node becomes electable.
ElectionHandoffTest.testElectionHandoff(
    rst, 0, 1, {stepDownPeriodSecs: 30, secondaryCatchUpPeriodSecs: 30});

rst.stopSet();
})();
