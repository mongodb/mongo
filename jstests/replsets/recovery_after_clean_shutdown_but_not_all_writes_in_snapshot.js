/**
 * Tests that fast metadata counts are correct after replication recovery following a clean
 * shutdown.
 *
 * @tags: [requires_persistence, requires_replication]
 */
(function() {
"use strict";

const rst = new ReplSetTest({
    name: "recoveryAfterCleanShutdown",
    nodes: 2,
    nodeOptions: {setParameter: {logComponentVerbosity: tojsononeline({storage: {recovery: 2}})}}
});
const nodes = rst.startSet();
rst.initiate();

const dbName = "recovery_clean_shutdown";
let primaryDB = rst.getPrimary().getDB(dbName);
const wMajority = {
    writeConcern: {w: "majority", wtimeout: ReplSetTest.kDefaultTimeoutMS}
};

// Create a collection that will have all of its writes in the stable checkpoint.
const collAllStableWrites = "allWritesInStableCheckpoint";
assert.commandWorked(primaryDB[collAllStableWrites].insert({_id: "dan"}, wMajority));
assert.commandWorked(primaryDB[collAllStableWrites].insert({_id: "judah"}, wMajority));
assert.commandWorked(primaryDB[collAllStableWrites].insert({_id: "vessy"}, wMajority));
assert.commandWorked(primaryDB[collAllStableWrites].insert({_id: "kyle"}, wMajority));

// Set up a collection with some writes that make it into the stable checkpoint.
const collSomeStableWrites = "someWritesInStableCheckpoint";
assert.commandWorked(primaryDB[collSomeStableWrites].insert({_id: "erjon"}, wMajority));
assert.commandWorked(primaryDB[collSomeStableWrites].insert({_id: "jungsoo"}, wMajority));

// Set up a collection whose creation is in the stable checkpoint, but will have no stable
// writes.
const collNoStableWrites = "noWritesInStableCheckpoint";
assert.commandWorked(primaryDB[collNoStableWrites].runCommand("create", wMajority));

// Wait for all oplog entries to enter the stable checkpoint on all secondaries.
rst.awaitLastOpCommitted();

// Disable snapshotting on all members of the replica set so that further operations do not
// enter the majority snapshot.
nodes.forEach(node => assert.commandWorked(node.adminCommand(
                  {configureFailPoint: "disableSnapshotting", mode: "alwaysOn"})));
const w1 = {
    writeConcern: {w: 1, wtimeout: ReplSetTest.kDefaultTimeoutMS}
};

// Set up a collection whose creation is not in the stable checkpoint.
const collNoStableCreation = "creationNotInStableCheckpoint";
assert.commandWorked(primaryDB[collNoStableCreation].runCommand("create", w1));

// Perform writes on collections that replicate to each node but do not enter the majority
// snapshot. These commands will be replayed during replication recovery during restart.
[collSomeStableWrites, collNoStableWrites, collNoStableCreation].forEach(
    coll => assert.commandWorked(
        primaryDB[coll].insert({_id: "insertedAfterSnapshottingDisabled"}, w1)));
rst.awaitReplication();

jsTestLog("Checking collection counts after snapshotting has been disabled");
rst.checkCollectionCounts();

// Perform a clean shutdown and restart. Note that the 'disableSnapshotting' failpoint will be
// unset on each node following the restart.
nodes.forEach(node => rst.restart(node));
rst.awaitNodesAgreeOnPrimary();
primaryDB = rst.getPrimary().getDB(dbName);

// Perform a majority write to ensure that both nodes agree on the majority commit point.
const collCreatedAfterRestart = "createdAfterRestart";
assert.commandWorked(
    primaryDB[collCreatedAfterRestart].insert({_id: "insertedAfterRestart", wMajority}));

// Fast metadata count should be correct after restart in the face of a clean shutdown.
jsTestLog("Checking collection counts after clean restart of all nodes");
rst.checkCollectionCounts();

rst.stopSet();
}());
