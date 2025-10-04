/**
 * This test uses a two-node replica set and exercises election handoff from one node to the other,
 * then back to the first one.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ElectionHandoffTest} from "jstests/replsets/libs/election_handoff.js";

const testName = "election_handoff_flip";
const numNodes = 2;
const rst = new ReplSetTest({name: testName, nodes: numNodes});
const nodes = rst.nodeList();
rst.startSet();

// Make sure there are no election timeouts firing for the duration of the test. This helps
// ensure that the test will only pass if the election handoff succeeds.
rst.initiate();

ElectionHandoffTest.testElectionHandoff(rst, 0, 1);
sleep(ElectionHandoffTest.stepDownPeriodSecs * 1000);
ElectionHandoffTest.testElectionHandoff(rst, 1, 0);

rst.stopSet();
