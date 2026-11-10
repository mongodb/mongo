/**
 * A node configured with oplogWriterThreadCount > 0 can finish initial sync and enter steady-state
 * replication with consistent data. Initial sync still writes oplog entries through the applier's
 * existing pool; the dedicated writer pool is used after initial sync.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kOplogWriterThreadCount = 4;

const rst = new ReplSetTest({
    name: "oplog_writer_initial_sync",
    nodes: 1,
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

if (!FeatureFlagUtil.isEnabled(primary, "ReduceMajorityWriteLatency")) {
    jsTestLog(
        "featureFlagReduceMajorityWriteLatency is disabled; skipping multi-threaded oplog " +
        "writer initial sync test.");
    rst.stopSet();
    quit();
}

const dbName = "test";
const collName = "oplog_writer_initial_sync";
const primaryColl = primary.getDB(dbName)[collName];

jsTestLog("Seeding the primary with a mix of small and large documents.");
{
    const smallBulk = primaryColl.initializeUnorderedBulkOp();
    for (let i = 0; i < 2000; i++) {
        smallBulk.insert({_id: i, kind: "small", v: i});
    }
    assert.commandWorked(smallBulk.execute());

    const bigString = "z".repeat(128 * 1024);  // 128KB -> byte-heavy batches during sync.
    const largeBulk = primaryColl.initializeUnorderedBulkOp();
    for (let i = 100000; i < 100100; i++) {
        largeBulk.insert({_id: i, kind: "large", payload: bigString});
    }
    assert.commandWorked(largeBulk.execute());
}
rst.awaitReplication();

jsTestLog("Adding a fresh node with oplogWriterThreadCount > 0 to trigger initial sync.");
const initialSyncNode = rst.add({
    setParameter: {
        oplogWriterThreadCount: kOplogWriterThreadCount,
        // Fail on the first initial sync error.
        numInitialSyncAttempts: 1,
    },
    rsConfig: {votes: 0, priority: 0},
});

// Reconfigure the set to include the new node, which begins initial sync.
rst.reInitiate();

// Keep writes flowing while the new node syncs and transitions to steady-state replication.
jsTestLog("Applying a concurrent write load during initial sync.");
const loadShell = startParallelShell(
    funWithArgs(function(dbName, collName) {
        const coll = db.getSiblingDB(dbName)[collName];
        const bigString = "w".repeat(64 * 1024);
        for (let i = 200000; i < 203000; i++) {
            assert.commandWorked(coll.insert({_id: i, kind: "load", payload: bigString}));
        }
        assert.commandWorked(coll.update({kind: "small"}, {$set: {touched: true}}, {multi: true}));
    }, dbName, collName),
    primary.port);

// Wait for the new node to finish initial sync and reach SECONDARY.
jsTestLog("Waiting for the new node to complete initial sync.");
rst.awaitSecondaryNodes(undefined, [initialSyncNode]);

loadShell();

const paramRes = assert.commandWorked(
    initialSyncNode.adminCommand({getParameter: 1, oplogWriterThreadCount: 1}));
assert.eq(kOplogWriterThreadCount, paramRes.oplogWriterThreadCount);

jsTestLog("Waiting for full replication and verifying data consistency across the set.");
rst.awaitReplication();
rst.waitForAllNewlyAddedRemovals();


rst.checkOplogs();
rst.checkReplicatedDataHashes();

initialSyncNode.setSecondaryOk();
assert.eq(primaryColl.find().itcount(),
          initialSyncNode.getDB(dbName)[collName].find().itcount(),
          "initial-synced node document count differs from primary");

jsTestLog("Initial sync with multi-threaded oplog writer completed consistently.");
rst.stopSet();
