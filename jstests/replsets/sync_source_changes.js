/**
 * Tests that when the current sync source no longer meets the strict criteria for being a sync
 * source, and there is another node which does meet those criteria, we will change sync source
 * (eventually).
 */

(function() {
"use strict";

load("jstests/replsets/rslib.js");             // reconfig
load("jstests/replsets/libs/sync_source.js");  // assertSyncSourceMatchesSoon

// We need to wait for a heartbeat from the secondary to the sync source, then run sync
// source selection, because:
// 1) The sync source changes only after retrieving a batch and
// 2) The sync source won't change if the secondary isn't behind the expected sync source, as
//    determined by heartbeats.
function assertSyncSourceChangesTo(rst, secondary, expectedSyncSource) {
    // Insert a document while 'secondary' is not replicating to force it to run
    // shouldChangeSyncSource.
    stopServerReplication(secondary);
    assert.commandWorked(
        rst.getPrimary().getDB("testSyncSourceChangesDb").getCollection("coll").insert({a: 1}, {
            writeConcern: {w: 1}
        }));
    const sourceId = rst.getNodeId(expectedSyncSource);
    // Waits for the secondary to see the expected sync source advance beyond it.
    assert.soon(function() {
        const status = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
        const appliedTimestamp = status.optimes.appliedOpTime.ts;
        const sourceMember = status.members.find((x) => x._id == sourceId);
        return timestampCmp(sourceMember.optime.ts, appliedTimestamp) > 0;
    });
    restartServerReplication(secondary);
    assertSyncSourceMatchesSoon(secondary, expectedSyncSource.host);
}

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
})();
