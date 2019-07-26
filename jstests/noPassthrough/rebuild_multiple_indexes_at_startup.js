/**
 * Tests that rebuilding multiple indexes at startup succeeds. Getting into this state requires
 * replication, but the code can execute in any mode.
 *
 * @tags: [requires_persistence, requires_replication]
 */
(function() {
"use strict";

const rst = new ReplSetTest({
    name: "rebuildMultipleIndexesAtStartup",
    nodes: 2,
    nodeOptions: {setParameter: {logComponentVerbosity: tojsononeline({storage: {recovery: 2}})}}
});
const nodes = rst.startSet();
rst.initiate();

if (!rst.getPrimary().adminCommand("serverStatus").storageEngine.supportsSnapshotReadConcern) {
    // Only snapshotting storage engines can pause advancing the stable timestamp allowing us
    // to get into a state where indexes exist, but the underlying tables were dropped.
    rst.stopSet();
    return;
}

let coll = rst.getPrimary().getDB("indexRebuild")["coll"];
assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}], {}, {writeConcern: {w: "majority"}}));
assert.eq(3, coll.getIndexes().length);
rst.awaitReplication(undefined, ReplSetTest.OpTimeType.LAST_DURABLE);

// Lock the index entries into a stable checkpoint by shutting down.
rst.stopSet(undefined, true);
rst.startSet(undefined, true);

// Disable snapshotting on all members of the replica set so that further operations do not
// enter the majority snapshot.
nodes.forEach(node => assert.commandWorked(node.adminCommand(
                  {configureFailPoint: "disableSnapshotting", mode: "alwaysOn"})));

// Dropping the index would normally modify the collection metadata and drop the
// table. Because we're not advancing the stable timestamp and we're going to crash the
// server, the catalog change won't take effect, but the WT table being dropped will.
coll = rst.getPrimary().getDB("indexRebuild")["coll"];
assert.commandWorked(coll.dropIndexes());
rst.awaitReplication();
rst.stopSet(9, true, {allowedExitCode: MongoRunner.EXIT_SIGKILL});

// Restarting the replica set should rebuild both indexes on both nodes. Just to be dropped
// again by replication recovery. Starting up successfully is a passing test run.
rst.startSet(undefined, true);
coll = rst.getPrimary().getDB("indexRebuild")["coll"];
assert.eq(1, coll.getIndexes().length);
rst.stopSet();
})();
