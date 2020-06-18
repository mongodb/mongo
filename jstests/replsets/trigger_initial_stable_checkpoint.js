/**
 * Ensure that we properly trigger a stable checkpoint when starting up a replica set node.
 *
 * We don't support unclean shutdowns with restarts into a last-stable binary.
 * @tags: [requires_persistence, multiversion_incompatible]
 */
(function() {
const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        // Disable background checkpoints: a zero value disables checkpointing.
        syncdelay: 0,
        setParameter: {logComponentVerbosity: tojson({storage: 2})}
    }
});
rst.startSet();
rst.initiate();

// By the time a node is primary it should have triggered a stable checkpoint. We subsequently kill
// and restart the node to check that the initial collections it created are durable in its
// checkpoint.
let primary = rst.getPrimary();
assert(checkLog.checkContainsOnce(primary, "Triggering the first stable checkpoint"));

jsTestLog("Kill and restart the node.");
rst.stop(0, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});
rst.start(0, undefined, true /* restart */);

jsTestLog("Waiting for the node to restart and become primary again.");
rst.getPrimary();

rst.stopSet();
}());
