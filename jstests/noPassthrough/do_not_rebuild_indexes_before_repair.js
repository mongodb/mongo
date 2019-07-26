/**
 * Tests that starting a mongod with --repair after an unclean shutdown does not attempt to rebuild
 * indexes before repairing the instance. Replication is used to get the database into a state where
 * an index has been dropped on disk, but still exists in the catalog.
 *
 * @tags: [requires_persistence, requires_replication, requires_majority_read_concern]
 */
(function() {
"use strict";

const dbName = "indexRebuild";
const collName = "coll";

const rst = new ReplSetTest({
    name: "doNotRebuildIndexesBeforeRepair",
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

let coll = rst.getPrimary().getDB(dbName)[collName];
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
// server, the catalog change won't take effect, but ident being dropped will.
coll = rst.getPrimary().getDB(dbName)[collName];
assert.commandWorked(coll.dropIndexes());
rst.awaitReplication();

let primaryDbpath = rst.getPrimary().dbpath;
let primaryPort = rst.getPrimary().port;
rst.stopSet(9, true, {allowedExitCode: MongoRunner.EXIT_SIGKILL});

// This should succeed in rebuilding the indexes, but only after the databases have been
// repaired.
assert.eq(0,
          runMongoProgram("mongod", "--repair", "--port", primaryPort, "--dbpath", primaryDbpath));

// Restarting the replica set would roll back the index drop. Instead we want to start a
// standalone and verify that repair rebuilt the indexes.
let mongod = MongoRunner.runMongod({dbpath: primaryDbpath, noCleanData: true});
assert.eq(3, mongod.getDB(dbName)[collName].getIndexes().length);

MongoRunner.stopMongod(mongod);
})();
