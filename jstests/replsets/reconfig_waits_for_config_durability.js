/**
 * Make sure that reconfig waits for the config document to be durable on nodes before returning.
 *
 * In this test, we disable all background checkpoints and journal flushes to eliminate those as a
 * cause for making writes durable. Then, we execute a reconfig on a replica set primary and wait
 * until the secondary has installed the new config. Finally, we SIGKILL the secondary and restart
 * it to verify that its config after restart is the same one it previously installed.
 *
 * @tags: [requires_persistence, requires_fcv_44]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0}}],
    nodeOptions: {
        // Turn up the syncdelay (in seconds) to effectively disable background checkpoints.
        syncdelay: 600,
        setParameter: {logComponentVerbosity: tojson({storage: 2})}
    },
    useBridge: true
});
rst.startSet();
rst.initiateWithHighElectionTimeout();

// We will kill the secondary after it installs and acknowledges a config to make sure it has made
// it durable. Disable journaling on the node so we are sure that the config write is flushed
// explicitly.
const secondaryNodeId = 1;
const journalFp = configureFailPoint(rst.nodes[secondaryNodeId], "pauseJournalFlusherThread");
journalFp.wait();

// Pause the secondary applier thread so it does not cause extra journal flushes. Make sure we wait
// for the fail point to be hit before proceeding.
configureFailPoint(rst.getSecondary(), "rsSyncApplyStop").wait();

// Do a reconfig and wait for propagation to all nodes.
jsTestLog("Doing a reconfig.");
let config = rst.getReplSetConfigFromNode();
const newConfigVersion = config.version + 1;
config.version = newConfigVersion;
assert.commandWorked(rst.getPrimary().adminCommand({replSetReconfig: config}));
rst.awaitNodesAgreeOnConfigVersion();

// Verify the node has the right config.
assert.eq(rst.getReplSetConfigFromNode(secondaryNodeId).version, newConfigVersion);
jsTestLog("Finished waiting for reconfig to propagate.");

// Isolate node 1 so that it does not automatically learn of a new config via heartbeat after
// restart.
rst.nodes[1].disconnect([rst.nodes[0]]);

jsTestLog("Kill and restart the secondary node.");
rst.stop(secondaryNodeId, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});
rst.start(secondaryNodeId, undefined, true /* restart */);
rst.awaitSecondaryNodes();

// Make sure that node 1 still has the config it acknowledged.
assert.eq(rst.getReplSetConfigFromNode(secondaryNodeId).version, newConfigVersion);

// Re-connect the node to let the test complete.
rst.nodes[1].reconnect([rst.nodes[0]]);
rst.stopSet();
}());
