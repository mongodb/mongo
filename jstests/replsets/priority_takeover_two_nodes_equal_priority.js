/**
 * Test to ensure that nodes with the highest priorities eventually become PRIMARY.
 *
 * 1. Initiate a 3 node replica set with node priorities of 3, 3 and 1 (default)
 * 2. Make sure that one of the highest priority nodes becomes PRIMARY.
 * 3. Step down the PRIMARY and confirm that the other high priority node becomes PRIMARY.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

let name = "priority_takeover_two_nodes_equal_priority";
let replTest = new ReplSetTest({name: name, nodes: [{rsConfig: {priority: 3}}, {rsConfig: {priority: 3}}, {}]});
replTest.startSet();
// We use the default electionTimeoutMillis to allow a priority takeover to occur in
// case a lower priority node gets elected.
replTest.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

jsTestLog("Waiting for one of the high priority nodes to become PRIMARY.");
let primary;
let primaryIndex = -1;
let defaultPriorityNodeIndex = 2;
assert.soon(
    function () {
        primary = replTest.getPrimary();
        replTest.nodes.find(function (node, index, array) {
            if (primary.host == node.host) {
                primaryIndex = index;
                return true;
            }
            return false;
        });
        return primaryIndex !== defaultPriorityNodeIndex;
    },
    "Neither of the high priority nodes was elected primary.",
    replTest.timeoutMS, // timeout
    1000, // interval
);

jsTestLog("Stepping down the current primary.");
assert.commandWorked(primary.adminCommand({replSetStepDown: 10 * 60 * 3, secondaryCatchUpPeriodSecs: 10 * 60}));

// Make sure the primary has stepped down.
assert.neq(primary, replTest.getPrimary());

// We expect the other high priority node to eventually become primary.
let expectedNewPrimaryIndex = primaryIndex === 0 ? 1 : 0;

jsTestLog("Waiting for the other high priority node to become PRIMARY.");
let expectedNewPrimary = replTest.nodes[expectedNewPrimaryIndex];
replTest.waitForState(expectedNewPrimary, ReplSetTest.State.PRIMARY);
replTest.stopSet();
