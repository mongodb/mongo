/*
 * Test that arbiters vote no in elections if they can see a healthy primary of equal or greater
 * priority to the candidate, preventing flapping during certain kinds of network partitions.
 *
 * 1.  Initiate a 3-node replica set with one arbiter (PSA) and a higher priority primary.
 * 2.  Create a network partition between the primary and secondary.
 * 3.  Wait long enough for the secondary to call for an election.
 * 4.  Verify the primary and secondary did not change.
 * 5.  Heal the partition.
 * 6.  Verify the primary and secondary did not change and are in the initial term.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

let name = "no_flapping_during_network_partition";

let replTest = new ReplSetTest({name: name, nodes: 3, useBridge: true});
let nodes = replTest.startSet();
let config = replTest.getReplSetConfig();
config.members[0].priority = 5;
config.members[2].arbiterOnly = true;
config.settings = {
    electionTimeoutMillis: 2000,
};
replTest.initiate(config);

function getTerm(node) {
    return node.adminCommand({replSetGetStatus: 1}).term;
}

replTest.waitForState(nodes[0], ReplSetTest.State.PRIMARY);

let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();
let initialTerm = getTerm(primary);

jsTestLog("Create a network partition between the primary and secondary.");
primary.disconnect(secondary);

jsTestLog("Wait long enough for the secondary to call for an election.");
checkLog.contains(secondary, "can see a healthy primary");
checkLog.contains(secondary, "Not running for primary");

jsTestLog("Verify the primary and secondary do not change during the partition.");
assert.eq(primary, replTest.getPrimary());
assert.eq(secondary, replTest.getSecondary());

checkLog.contains(secondary, "Not running for primary");

jsTestLog("Heal the partition.");
primary.reconnect(secondary);

replTest.stopSet();
