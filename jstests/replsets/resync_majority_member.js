/**
 * This test resyncs a majority member against a minority node, so that it no longer has
 * a write it originally helped commit. It then switches primaries and begins a new branch
 * of history, so that same write is now in the minority. The only remaining member to still
 * have that write is forced to (try to) roll back, and it crashes as it refuses to roll back
 * majority-committed writes.
 *
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/storage_engine_utils.js");
load("jstests/libs/write_concern_util.js");

TestData.skipCheckDBHashes = true;  // the set is not consistent when we shutdown the test

const dbName = "testdb";
const collName = "testcoll";

const name = jsTestName();
const rst = new ReplSetTest({
    name: name,
    nodes: [{}, {}, {rsConfig: {priority: 0}}],
    useBridge: true,
    settings: {chainingAllowed: false, catchupTimeoutMillis: 0 /* disable primary catchup */},
});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);
// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
rst.awaitReplication();
assert.commandWorked(primaryColl.insert({"starting": "doc", writeConcern: {w: 3}}));

/**
 * Node 1: is primary, will roll back (included in the majority)
 * Node 2: node to roll back against (minority node)
 * Node 3: node to resync (originally included in majority, resyncs and loses write)
 */

const rollbackNode = primary;
const syncSource = rst.getSecondaries()[0];
let resyncNode = rst.getSecondaries()[1];

// Disable replication on node 2 so that only nodes 1 and 3 have the next write.
stopServerReplication(syncSource);

const disappearingDoc = {
    "harry": "houdini"
};
assert.commandWorked(primaryColl.insert(disappearingDoc, {writeConcern: {w: "majority"}}));

// Isolate the old primary so it cannot try to pass on its write again.
rollbackNode.disconnect(syncSource);
rollbackNode.disconnect(resyncNode);

// Resync the last node against the minority member. We will lose the write on that node.
resyncNode = rst.restart(resyncNode, {
    startClean: true,
    setParameter: {
        "failpoint.initialSyncHangBeforeFinish": tojson({mode: "alwaysOn"}),
        "failpoint.forceSyncSourceCandidate":
            tojson({mode: "alwaysOn", data: {"hostAndPort": syncSource.host}}),
        "numInitialSyncAttempts": 1
    }
});

assert.commandWorked(resyncNode.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeFinish",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));
assert.commandWorked(
    resyncNode.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));

assert.commandWorked(
    rollbackNode.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
rst.waitForState(rollbackNode, ReplSetTest.State.SECONDARY);

restartServerReplication(syncSource);

// Now elect node 2, the minority member.
assert.commandWorked(syncSource.adminCommand({replSetStepUp: 1}));
assert.eq(syncSource, rst.getPrimary());
assert.commandWorked(syncSource.getDB(dbName).getCollection(collName).insert(
    {"new": "data"}, {writeConcern: {w: "majority"}}));

// This failpoint will only be hit if the node's rollback common point is before the replication
// commit point, which triggers an invariant. This failpoint is used to verify the invariant
// will be hit without having to search the logs.
let rollbackCommittedWritesFailPoint;
if (storageEngineIsWiredTigerOrInMemory()) {
    rollbackCommittedWritesFailPoint =
        configureFailPoint(rollbackNode, "rollbackToTimestampHangCommonPointBeforeReplCommitPoint");
} else {
    rollbackCommittedWritesFailPoint =
        configureFailPoint(rollbackNode, "rollbackViaRefetchHangCommonPointBeforeReplCommitPoint");
}

// Node 1 will have to roll back to rejoin the set. It will crash as it will refuse to roll back
// majority committed data.
rollbackNode.reconnect(syncSource);
rollbackNode.reconnect(resyncNode);

assert.soonNoExcept(() => {
    rollbackCommittedWritesFailPoint.wait();
    return true;
}, `failed to wait for fail point ${rollbackCommittedWritesFailPoint.failPointName}`);

rollbackCommittedWritesFailPoint.off();

// Observe that the old write does not exist anywhere in the set.
syncSource.setSecondaryOk();
resyncNode.setSecondaryOk();
assert.eq(0, syncSource.getDB(dbName)[collName].find(disappearingDoc).itcount());
assert.eq(0, resyncNode.getDB(dbName)[collName].find(disappearingDoc).itcount());

// We expect node 1 to have crashed.
rst.stop(0, undefined, {allowedExitCode: MongoRunner.EXIT_ABORT});
rst.stopSet();
})();
