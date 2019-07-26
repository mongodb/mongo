/**
 * Restarting the catalog will destroy and recreate all database/collection/index objects from the
 * storage engine state. However, some fields like the `minimumVisibleSnapshot` timestamp are not
 * persisted to storage. This value is typically rehydrated when performing replication
 * recovery. However there are cases where reads can be at a timestamp prior to where replication
 * recovery begins. Those are fixed by copying the previous value over from the destroyed catalog
 * object to the recreated one.
 *
 * This test verifies the collection's minimum visible snapshot timestamp is appropriately copied
 * over.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

let replSet = new ReplSetTest({name: "server35317", nodes: 1});
replSet.startSet();
replSet.initiate();

let prim = replSet.getPrimary();
let beforeIndexBuild = assert.commandWorked(prim.adminCommand({
    configureFailPoint: "WTPreserveSnapshotHistoryIndefinitely",
    mode: "alwaysOn"
}))["operationTime"];
assert.commandWorked(prim.getDB("test").coll.insert({c: 1}));
assert.commandWorked(prim.getDB("test").coll.createIndex({c: 1}));
assert.commandWorked(prim.adminCommand({restartCatalog: 1}));

let session = prim.startSession({causalConsistency: false});
let sessionDb = session.getDatabase("test");
// Prior to fixing SERVER-35317, this would crash a debug build, or return success on a
// non-debug build. Now it should return an error. Specifically, this fails because we're
// trying to read behind the minimum visible snapshot timestamp for the `test.coll`
// collection.
assert.commandFailed(sessionDb.runCommand({
    find: "coll",
    filter: {c: 1},
    readConcern: {level: "snapshot", atClusterTime: beforeIndexBuild},
    txnNumber: NumberLong(0)
}));

session.endSession();
replSet.stopSet();
})();
