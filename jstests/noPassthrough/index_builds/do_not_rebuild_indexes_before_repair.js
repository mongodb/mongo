/**
 * Tests that starting a mongod with --repair after an unclean shutdown does not attempt to rebuild
 * indexes before repairing the instance. Replication is used to get the database into a state where
 * an index has been dropped on disk, but still exists in the catalog.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = jsTestName();
const collName = "coll";

const rst = new ReplSetTest({
    name: "doNotRebuildIndexesBeforeRepair",
    nodes: 2,
    nodeOptions: {setParameter: {logComponentVerbosity: tojsononeline({storage: {recovery: 2}})}},
});
const nodes = rst.startSet();
rst.initiate();

if (!rst.getPrimary().adminCommand("serverStatus").storageEngine.supportsSnapshotReadConcern) {
    // Only snapshotting storage engines can pause advancing the stable timestamp allowing us
    // to get into a state where indexes exist, but the underlying tables were dropped.
    rst.stopSet();
    quit();
}

let primary = rst.getPrimary();
let testDB = primary.getDB(dbName);
let coll = testDB.getCollection(collName);
// The default WC is majority and disableSnapshotting failpoint will prevent satisfying any majority
// writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

assert.commandWorked(
    testDB.runCommand({
        createIndexes: collName,
        indexes: [
            {key: {a: 1}, name: "a_1"},
            {key: {b: 1}, name: "b_1"},
        ],
        writeConcern: {w: "majority"},
    }),
);
assert.eq(3, coll.getIndexes().length);
rst.awaitReplication(undefined, ReplSetTest.OpTimeType.LAST_DURABLE);

// Lock the index entries into a stable checkpoint by shutting down.
rst.stopSet(undefined, true);
rst.startSet(undefined, true);

// Disable snapshotting on all members of the replica set so that further operations do not
// enter the majority snapshot.
nodes.forEach((node) =>
    assert.commandWorked(node.adminCommand({configureFailPoint: "disableSnapshotting", mode: "alwaysOn"})),
);

// Dropping the index would normally modify the collection metadata and drop the
// table. Because we're not advancing the stable timestamp and we're going to crash the
// server, the catalog change won't take effect, but ident being dropped will.
primary = rst.getPrimary();
testDB = primary.getDB(dbName);
coll = testDB.getCollection(collName);
assert.commandWorked(coll.dropIndexes());
rst.awaitReplication();

let primaryDbpath = rst.getPrimary().dbpath;
let primaryPort = rst.getPrimary().port;
rst.stop(0, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});
rst.stopSet(undefined, true);

// This should succeed in rebuilding the indexes, but only after the databases have been
// repaired.
assert.eq(0, runMongoProgram("mongod", "--repair", "--port", primaryPort, "--dbpath", primaryDbpath));

// Restarting the replica set would roll back the index drop. Instead we want to start a
// standalone and verify that repair rebuilt the indexes.
let mongod = MongoRunner.runMongod({dbpath: primaryDbpath, noCleanData: true});
assert.eq(3, mongod.getDB(dbName)[collName].getIndexes().length);

MongoRunner.stopMongod(mongod);
