/*
 * SERVER-87919: exhibits the unsafe state produced when startup-recovery-for-restore
 * persists an *unstable* checkpoint and the node then crashes before its first
 * stable checkpoint. On restart, WiredTiger's Rollback-to-Stable skips tables
 * with timestamped updates (because the recovery checkpoint is unstable), and
 * replication recovery then reapplies oplog entries whose commit timestamps are
 * already physically present in the durable table --- WT surfaces this as an
 * out-of-order timestamped write (AF-618, AF-3985, AF-9648).
 *
 * This test pins the *current* unsafe behavior so that the SERVER-87919 fix
 * (suppressing unstable checkpoints during recovery for restore / promoting
 * them to stable checkpoints anchored at the advanced stable timestamp) can be
 * validated by inverting the assertions below.
 *
 * Companion TLA+ proof: src/mongo/tla_plus/Replication/UnstableCheckpointRecovery
 *
 * @tags: [
 *   requires_wiredtiger,
 *   requires_persistence,
 *   requires_replication,
 *   requires_majority_read_concern,
 *   # Restore semantics are not expected to be exercised mid-upgrade.
 *   multiversion_incompatible,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const SIGKILL = 9;
const dbName = TestData.testName;
const collName = "testcoll";
const sentinelCollName = "sentinelcoll";

// Verbose recovery + checkpoint logging so the unstable-checkpoint event and
// the subsequent out-of-order write surface cleanly in the test log.
const logLevel = tojson({storage: {recovery: 2, wt: {wtCheckpoint: 1}}});

const rst = new ReplSetTest({
    nodes: [{}, {}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    settings: {chainingAllowed: false},
});

const startParams = {
    logComponentVerbosity: logLevel,
    replBatchLimitOperations: 100,
};
const nodes = rst.startSet({setParameter: startParams});
let restoreNode = nodes[1];
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB(dbName);
const coll = db[collName];
const paddingStr = "X".repeat(64);

// The default WC is majority and this test deliberately holds back the
// majority commit point on the restore node.
assert.commandWorked(primary.adminCommand({
    setDefaultRWConcern: 1,
    defaultWriteConcern: {w: 1},
    writeConcern: {w: "majority"},
}));

// Pre-load some documents that will participate in the unsafe replay.
const nDocs = 50;
{
    const bulk = coll.initializeUnorderedBulkOp();
    for (let id = 1; id <= nDocs; id++) {
        bulk.insert({_id: id, paddingStr});
    }
    bulk.execute();
}
rst.awaitReplication();

// Freeze the stable timestamp on the restore node so subsequent writes apply
// past the stable boundary --- mirrors a real lagged-stable restore.
const holdOpTime = assert.commandWorked(
    db.runCommand({find: collName, limit: 1}),
).operationTime;
assert.commandWorked(restoreNode.adminCommand({
    configureFailPoint: "holdStableTimestampAtSpecificTimestamp",
    mode: "alwaysOn",
    data: {timestamp: holdOpTime},
}));

// Hold back the majority commit point on the other secondaries so the
// restore node's stable timestamp cannot advance via the normal path.
const stopReplProducer2 = configureFailPoint(nodes[2], "stopReplProducer");
const stopReplProducer3 = configureFailPoint(nodes[3], "stopReplProducer");

// Generate many same-key updates so the durable table accumulates a chain of
// timestamped writes per key. After unstable-checkpoint recovery, replay of
// any one of these will land on a same-key, same-or-earlier timestamp.
const nUpdates = 400;
jsTestLog(`Issuing ${nUpdates} updates against held-stable restore node.`);
{
    const bulk = coll.initializeUnorderedBulkOp();
    for (let u = 1; u <= nUpdates; u++) {
        const id = ((u - 1) % nDocs) + 1;
        const setDoc = {};
        setDoc["u" + u] = u;
        bulk.find({_id: id}).updateOne({$set: setDoc});
    }
    bulk.execute();
}

// A sentinel write whose presence we will use to detect successful replay of
// the affected oplog suffix on the recovered node.
assert.commandWorked(db.runCommand({
    insert: sentinelCollName,
    documents: [{_id: "s_before_unstable"}],
}));

rst.awaitReplication(undefined, ReplSetTest.OpTimeType.LAST_DURABLE, [restoreNode]);

// ---------------------------------------------------------------------------
// Step 1: shut the restore node down in standalone-restore mode so it persists
// an unstable checkpoint (takeUnstableCheckpointOnShutdown). This is the write
// SERVER-87919 wants to stop emitting.
// ---------------------------------------------------------------------------
jsTestLog("Restarting restore node with --startupRecoveryForRestore and " +
          "takeUnstableCheckpointOnShutdown=true (legacy unsafe path).");
restoreNode = rst.restart(restoreNode, {
    noReplSet: true,
    setParameter: Object.merge(startParams, {
        startupRecoveryForRestore: true,
        recoverFromOplogAsStandalone: true,
        takeUnstableCheckpointOnShutdown: true,
    }),
});

// The minValid document still carries appliedThrough --- the load-bearing
// signal that the persisted checkpoint is unstable.
let minValid = restoreNode.getCollection("local.replset.minvalid").findOne();
assert(minValid.hasOwnProperty("begin"),
       "expected appliedThrough/begin present after unstable-checkpoint " +
       "recovery: " + tojson(minValid));

// ---------------------------------------------------------------------------
// Step 2: bring the node back as a replica-set member but disable snapshotting
// so no stable checkpoint can be taken before the crash. Replicate one more
// oplog entry so we have a fresh suffix to replay after restart.
// ---------------------------------------------------------------------------
jsTestLog("Restarting restore node in repl-set mode with snapshotting disabled.");
restoreNode = rst.restart(restoreNode, {
    noReplSet: false,
    setParameter: Object.merge(startParams, {
        "failpoint.disableSnapshotting": "{'mode':'alwaysOn'}",
    }),
});
rst.awaitSecondaryNodes(undefined, [restoreNode]);

// Sanity: fsync should refuse before the first stable checkpoint, because
// otherwise we would emit *another* unstable checkpoint with null
// appliedThrough --- this is the existing canary.
jsTestLog("fsync must fail before the first stable checkpoint exists.");
assert.commandFailed(restoreNode.adminCommand({fsync: 1}));

assert.commandWorked(db.runCommand({
    insert: sentinelCollName,
    documents: [{_id: "s_after_unstable"}],
}));
rst.awaitReplication(undefined, ReplSetTest.OpTimeType.LAST_DURABLE, [restoreNode]);

// ---------------------------------------------------------------------------
// Step 3: SIGKILL before any stable checkpoint exists. On restart, the only
// recovery checkpoint available is the *unstable* one from Step 1 --- RTS will
// skip tables with timestamped updates, and oplog replay will reapply entries
// whose ts is already physically present in the table. This is the AF-618
// state SERVER-87919 must prevent.
// ---------------------------------------------------------------------------
jsTestLog("SIGKILL restore node before first stable checkpoint.");
rst.stop(restoreNode,
         SIGKILL,
         {allowedExitCode: MongoRunner.EXIT_SIGKILL},
         {forRestart: true});

jsTestLog("Restarting restore node again. Replication recovery from the " +
          "unstable checkpoint is the unsafe codepath under SERVER-87919.");
restoreNode = rst.start(
    restoreNode,
    {noReplSet: false, setParameter: startParams},
    /* restart */ true,
);
rst.awaitSecondaryNodes(undefined, [restoreNode]);

// The node either:
//   (a) comes back, having silently reapplied oplog entries whose effects were
//       already physically present in the durable table (current behavior;
//       latent corruption / surfaces later as duplicate-key / out-of-order
//       writes, cf. AF-3985 / AF-9648), or
//   (b) crashed during replay with a WT out-of-order timestamped write
//       (AF-618 tassert).
// Either is unsafe; the SERVER-87919 fix should make both unreachable. The
// assertions below pin the *observable* unsafe signature so the future fix
// flips them.

const restoreDb = restoreNode.getDB(dbName);

// All sentinels are present --- the replay did happen end-to-end.
assert.eq(2,
          restoreDb[sentinelCollName].find().itcount(),
          "expected both sentinels visible after restart-from-unstable replay");

// Pre-fix expectation: the persisted recovery checkpoint was unstable, so
// minValid still records an appliedThrough cursor across the restart boundary.
// Post-fix expectation: the node should have come up from a stable checkpoint
// and minValid.begin should be absent. Flip this assertion when SERVER-87919
// lands.
minValid = restoreNode.getCollection("local.replset.minvalid").findOne();
jsTestLog("minValid after unsafe-path recovery: " + tojson(minValid));
assert(minValid.hasOwnProperty("begin"),
       "SERVER-87919 pre-fix: appliedThrough/begin still present because the " +
       "recovery checkpoint was unstable. When the fix lands, this assertion " +
       "should be inverted (begin absent <=> recovered from stable checkpoint).");

// Cleanup. Allow majority to advance so the set can shut down cleanly with
// dbHash verification.
stopReplProducer2.off();
stopReplProducer3.off();
rst.stopSet();
