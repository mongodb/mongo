/**
 * Tests that when the current sync source no longer meets the strict criteria for being a sync
 * source, and there is another node which does meet those criteria, we will change sync source
 * (eventually).
 *
 * @tags: [
 *   __TEMPORARILY_DISABLED__,
 * ]
 */

(function() {
"use strict";

load("jstests/replsets/rslib.js");             // reconfig
load("jstests/replsets/libs/sync_source.js");  // assertSyncSourceMatchesSoon

// Start RST with only one voting node, node 0 -- this will be the only valid voting node and sync
// source
const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]});
rst.startSet();
rst.initiate();

// Make sure that node 0 is primary as expected
const primary = rst.getPrimary();
assert.eq(primary, rst.nodes[0]);

// Add a new voting node, node 2 -- voting nodes will choose voting nodes as sync sources.
const newNode = rst.add({});
rst.reInitiate();
rst.waitForState(newNode, ReplSetTest.State.SECONDARY);
rst.awaitReplication();
rst.awaitSecondaryNodes();

// Assure that node 2 will set node 0 as its sync source, since it is the best option.
assertSyncSourceMatchesSoon(newNode, rst.nodes[0].host);

// Make node 1 a voter so that it will be a valid option for sync source
let cfg = rst.getReplSetConfigFromNode();
cfg.members[1].priority = 1;
cfg.members[1].votes = 1;
reconfig(rst, cfg);

// Force a stepup of node 1 -- we need to step node 0 down so that we can set it as a non-voter
// without causing errors.
rst.stepUp(rst.nodes[1]);

// Make node 0 a nonvoter so that it will be an invalid option for sync source
cfg = rst.getReplSetConfigFromNode();
cfg.members[0].priority = 0;
cfg.members[0].votes = 0;
reconfig(rst, cfg);

// Run this repeatedly, as sometimes the stop, insert, restart won't cause the sync source to be
// switched correctly due to transient issues with the sync source we want to switch to.
assert.soon(() => {
    // Insert a document while newNode is not replicating to force it to run shouldChangeSyncSource
    stopServerReplication(newNode);
    assert.commandWorked(
        rst.getPrimary().getDB("testSyncSourceChangesDb").getCollection("coll").insert({a: 1}, {
            writeConcern: {w: 1}
        }));
    restartServerReplication(newNode);
    try {
        assertSyncSourceMatchesSoon(newNode,
                                    cfg.members[1].host,
                                    undefined /* msg */,
                                    5 * 1000 /* timeout */,
                                    undefined /* interval */,
                                    {runHangAnalyzer: false});
        return true;
    } catch (e) {
        return false;
    }
});

rst.stopSet();
})();
