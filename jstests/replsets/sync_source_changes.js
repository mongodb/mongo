/**
 * Tests that when the current sync source no longer meets the strict criteria for being a sync
 * source, and there is another node which does meet those criteria, we will change sync source
 * (eventually).
 */

import {assertSyncSourceChangesTo} from "jstests/replsets/libs/sync_source.js";
import {reconfig} from "jstests/replsets/rslib.js";

// Replication verbosity 2 includes the sync source change debug logs.
TestData["setParameters"]["logComponentVerbosity"]["replication"]["verbosity"] = 2;

// Start RST with only one voting node, node 0 -- this will be the only valid voting node and sync
// source
const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]});
rst.startSet();
rst.initiate();

// Make sure that node 0 is primary as expected
const primary = rst.getPrimary();
assert.eq(primary, rst.nodes[0]);

// Add a new voting node, node 2 -- voting nodes will choose voting nodes as sync sources.
jsTestLog("Adding node 2");
const newNode = rst.add({});
rst.reInitiate();
rst.waitForState(newNode, ReplSetTest.State.SECONDARY);
rst.awaitReplication();
rst.awaitSecondaryNodes();

// Wait for the new node to no longer be newlyAdded, so that it becomes a voting node.
rst.waitForAllNewlyAddedRemovals();

// Assure that node 2 will set node 0 as its sync source, since it is the best option.
assertSyncSourceChangesTo(rst, newNode, rst.nodes[0]);

// Make node 1 a voter so that it will be a valid option for sync source
let cfg = rst.getReplSetConfigFromNode();
cfg.members[1].priority = 1;
cfg.members[1].votes = 1;
reconfig(rst, cfg);

// Force a stepup of node 1 -- we need to step node 0 down so that we can set it as a non-voter
// without causing errors.
jsTestLog("Stepping up node 1");
rst.stepUp(rst.nodes[1]);

jsTestLog("Reconfiguring node 0 as nonvoter");
// Make node 0 a nonvoter so that it will be an invalid option for sync source
cfg = rst.getReplSetConfigFromNode();
cfg.members[0].priority = 0;
cfg.members[0].votes = 0;
reconfig(rst, cfg);
jsTestLog("Reconfig complete");

assertSyncSourceChangesTo(rst, newNode, rst.nodes[1]);

rst.stopSet();
