/*
 * Tests that startup recovery for restore recovers correctly after a crash mid oplog replay batch.
 *
 * The node holds its stable timestamp back, applies a workload of repeated updates to a single
 * record, then restarts with startupRecoveryForRestore and is SIGKILLed after a checkpoint has
 * captured a batch's writes but before the recovery loop advances appliedThrough past that batch.
 * On resume, recovery must reach a consistent state and read back every update.
 *
 * The workload uses a single _id so that a recovery batch holds multiple updates to the same record
 * at increasing timestamps.
 *
 * @tags: [requires_wiredtiger, requires_persistence, requires_replication,
 * requires_majority_read_concern, multiversion_incompatible]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const SIGKILL = 9;

const dbName = TestData.testName;
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
const collName = "testcoll";
const coll = db[collName];
const targetId = "target";

assert.commandWorked(coll.insert({_id: targetId, n: 0}));
rst.awaitReplication();

const holdOpTime = assert.commandWorked(db.runCommand({find: collName, limit: 1})).operationTime;

// Pin the stable timestamp on the restore node so its checkpoint stays at holdOpTime while the oplog
// past that point grows. This is to set up a lagged stable during recover for restore.
assert.commandWorked(
    restoreNode.adminCommand({
        configureFailPoint: "holdStableTimestampAtSpecificTimestamp",
        mode: "alwaysOn",
        data: {timestamp: holdOpTime},
    }),
);

// Updates to the same _id at increasing timestamps.
const nUpdates = 1000;
jsTestLog(
    "Applying " +
        nUpdates +
        " updates to a single document with stable pinned on the restore node.",
);
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= nUpdates; i++) {
    bulk.find({_id: targetId}).updateOne({$set: {n: i}});
}
bulk.execute();
rst.awaitReplication(undefined, undefined, [restoreNode]);

// Phase 1: restart in restore mode, freeze a batch after its writes commit but before appliedThrough
// advances, let a checkpoint capture that batch's data, then crash uncleanly.
jsTestLog("Restarting restore node with --startupRecoveryForRestore, frozen mid-batch.");
rst.stop(restoreNode, undefined, undefined, {forRestart: true});
clearRawMongoProgramOutput();
rst.start(
    restoreNode,
    {
        noReplSet: true,
        waitForConnect: false,
        syncdelay: 1, // Take checkpoints once per second.
        setParameter: Object.merge(startParams, {
            startupRecoveryForRestore: true,
            recoverFromOplogAsStandalone: true,
            takeUnstableCheckpointOnShutdown: true,
            "failpoint.pauseBatchApplicationBeforeCompletion": tojson({mode: "alwaysOn"}),
        }),
    },
    true /* restart */,
);

// Frozen batch's writes are committed with appliedThrough lagging.
let subStr = "pauseBatchApplicationBeforeCompletion fail point enabled";
assert.soon(() => rawMongoProgramOutput(subStr).search(subStr) !== -1);

// Wait for a full checkpoint to complete after the freeze.
// Checkpoints are serialized, so observing the "saving checkpoint snapshot min" line twice
// means the first checkpoint has completed.
subStr = "WT_VERB_CHECKPOINT.*saving checkpoint snapshot min";
clearRawMongoProgramOutput();
assert.soon(() => rawMongoProgramOutput(subStr).search(subStr) !== -1);
clearRawMongoProgramOutput();
assert.soon(() => rawMongoProgramOutput(subStr).search(subStr) !== -1);

jsTestLog("Crashing the restore node uncleanly while the batch is frozen.");
rst.stop(restoreNode, SIGKILL, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});

// Phase 2: resume restore recovery. Pre-fix (unstable checkpoints) this aborts with the WiredTiger
// out-of-order timestamp error during startup. With the fix (stable checkpoints) the node recovers
// from a stable checkpoint and starts cleanly.
jsTestLog("Resuming restore recovery; expecting clean startup once SERVER-87919 lands.");
restoreNode = rst.start(
    restoreNode,
    {
        noReplSet: true,
        setParameter: Object.merge(startParams, {
            startupRecoveryForRestore: true,
            recoverFromOplogAsStandalone: true,
            takeUnstableCheckpointOnShutdown: true,
        }),
    },
    true /* restart */,
);

// All updates must be present, and recovery must not reapply entries already in the checkpoint.
assert.eq(nUpdates, restoreNode.getDB(dbName)[collName].findOne({_id: targetId}).n);

jsTestLog("Rejoining the restore node to the set and verifying consistency.");
restoreNode = rst.restart(restoreNode, {noReplSet: false, setParameter: startParams});
rst.awaitSecondaryNodes(undefined, [restoreNode]);
rst.stopSet();
