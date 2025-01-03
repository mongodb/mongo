/**
 * Tests that when a node hit "sync source resolver shut down while probing candidate",
 * its sync source is left unset.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {assertSyncSourceChangesTo} from "jstests/replsets/libs/sync_source.js";

// Start RST with only one node.
const rst = new ReplSetTest({nodes: [{}]});
rst.startSet();
rst.initiate();

// Make sure that node 0 is primary as expected
const primary = rst.getPrimary();
assert.eq(primary, rst.nodes[0]);

// Add a new voting node, node 1 -- voting nodes will choose voting nodes as sync sources.
jsTestLog("Adding node 1");
const newNode = rst.add({});

rst.reInitiate();
rst.waitForState(newNode, ReplSetTest.State.SECONDARY);
rst.awaitReplication();
rst.awaitSecondaryNodes();

// Wait for the new node to no longer be newlyAdded, so that it becomes a voting node.
rst.waitForAllNewlyAddedRemovals();

// Assure that node 1 will set node 0 as its sync source.
assertSyncSourceChangesTo(rst, newNode, rst.nodes[0]);

jsTestLog("Node 1 is syncing from Node 0");

// Simulate a shut down.
let failfirstOplogEntryFetcherCallback =
    configureFailPoint(newNode, "failfirstOplogEntryFetcherCallback");

jsTestLog("Calling replSetSyncFrom while failfirstOplogEntryFetcherCallback is enabled");
assert.commandWorked(newNode.adminCommand({replSetSyncFrom: primary.name}));

failfirstOplogEntryFetcherCallback.wait();

jsTestLog("Waiting for syncSource to be unset");
assert.soon(function() {
    const {members} = assert.commandWorked(newNode.adminCommand({replSetGetStatus: 1}));
    for (const member of members) {
        if (member.syncSourceId != -1) {
            return false;
        }
    }

    return true;
});

failfirstOplogEntryFetcherCallback.off();

rst.stopSet();
