/**
 * This test ensures that logs and metrics capture for the following scenario will be sufficient to
 * diagnose in real scenarios.
 *
 * In the authoritative shard model a live replication rollback on a shard triggers
 * ShardingRecoveryService to reset ALL in-memory CollectionShardingRuntime (CSR) filtering
 * metadata. Once CRUD resumes, every affected namespace must re-fetch its filtering metadata, which
 * shows up as latency.
 *
 * The test asserts that the server emits enough stats and logs for an operator to reconstruct the
 * story: "these collections got slow right after a rollback because the shard threw away all of its
 * filtering metadata and had to refresh it." We verify three diagnostic surfaces:
 *   1. Logs: the reset and refresh log IDs.
 *   2. shardingStatistics.collectionShardingMetadataStatistics counters, which quantify the whole
 *      story.
 *   3. The disk-read time spent rebuilding the CSRs, tallied against the delay we purposefully
 *      injected via a test-only per-chunk sleep on the recovery path.
 *   4. The customer-visible latency: the resumed CRUD, routed to the rolled-back (now primary) node
 *      whose CSRs were cleared, is materially slower while those CSRs are rebuilt from disk.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_replication,
 *   requires_mongobridge,
 *   requires_persistence,
 *   requires_fcv_90,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";
import {restartReplSetReplication} from "jstests/libs/write_concern_util.js";

const dbName = "csrRollbackDB";
const collNames = ["coll0", "coll1", "coll2", "coll3"];
const forceResetFailPointName = "forceTriggerShardingRecoveryRollbackReset";
const sleepPerChunkFailPointName = "sleepPerChunkDuringCollectionMetadataDiskRecovery";

// We roll back the config server's replica set, which is also a data-bearing shard.
// This is deliberate: RollbackTest issues 'setDefaultRWConcern' directly against the RS primary,
// which dedicated shard nodes reject but config server nodes accept. Using a config shard means the
// rolled-back RS both owns filtering metadata for user collections (so ShardingRecoveryService
// resets its CSR) and tolerates the RollbackTest setup.
// For the live rollback to be tested, the RS needs 3 nodes with a priority:0 tiebreaker, chaining
// disabled, and a "forever" election timeout; mongobridge is required.
const st = new ShardingTest({
    shards: 1,
    mongos: 1,
    configShard: true,
    config: [{}, {}, {rsConfig: {priority: 0}}],
    configReplSetTestOptions: {
        settings: {chainingAllowed: false, electionTimeoutMillis: ReplSetTest.kForeverMillis},
    },
    // Disable the periodic sharded index consistency checker on the config server primary. It
    // otherwise sweeps the sharded collections sequentially with an $indexStats aggregate, which
    // would pre-warm a collection's CSR before the test issues a targeted query to do so.
    other: {
        useBridge: true,
        configOptions: {setParameter: {enableShardedIndexConsistencyCheck: false}},
    },
});

const mongos = st.s0;
const adminDB = mongos.getDB("admin");
const testDB = mongos.getDB(dbName);

assert.commandWorked(
    adminDB.runCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);

// Shards 'ns' on {_id: 1} and splits it into 'numChunks' chunks, each holding 'docsPerChunk' docs.
function shardCollectionWithManyChunks(ns, numChunks, docsPerChunk) {
    assert.commandWorked(adminDB.runCommand({shardCollection: ns, key: {_id: 1}}));
    for (let i = 1; i < numChunks; i++) {
        assert.commandWorked(adminDB.runCommand({split: ns, middle: {_id: i * docsPerChunk}}));
    }
}

// Helper to do a targeted read and return the latency observed at the shell.
function timedReadWorker(mongosHost, dbName, collName, expectedCount) {
    const db = new Mongo(mongosHost).getDB(dbName);
    const start = Date.now();
    assert.eq(expectedCount, db[collName].find().itcount());
    return {collName, latencyMillis: Date.now() - start};
}

// Spawns 'readersPerCollection' concurrent readers for every namespace (so each collection gets
// multiple simultaneous clients), and returns the per-thread {collName, latencyMillis} results.
function runParallelReads(mongosHost, dbName, collNames, readersPerCollection, expectedCount) {
    const threads = [];
    for (const collName of collNames) {
        for (let r = 0; r < readersPerCollection; r++) {
            const thread = new Thread(timedReadWorker, mongosHost, dbName, collName, expectedCount);
            thread.start();
            threads.push(thread);
        }
    }
    return threads.map((thread) => {
        thread.join();
        return thread.returnData();
    });
}

// Keep low for a quick jstest.
const kChunksPerCollection = 10;
// Docs seeded into each chunk during the baseline.
const kDocsPerChunk = 2;
// Docs seeded into each collection during the baseline (two per chunk).
const kBaselineDocCount = kDocsPerChunk * kChunksPerCollection;
// Concurrent readers per collection.
const kReadersPerCollection = 2;

// --- Step 1: Setup and bring to a steady state.
for (const collName of collNames) {
    const ns = dbName + "." + collName;
    shardCollectionWithManyChunks(ns, kChunksPerCollection, kDocsPerChunk);

    const coll = testDB[collName];
    assert.commandWorked(
        coll.insert(Array.from({length: kBaselineDocCount}, (_, i) => ({_id: i, v: i}))),
    );
}

// Warm up every CSR under the same concurrent-reader workload used post-rollback, so the two phases
// are directly comparable. The CSRs are already populated, so these reads are fast.
const baselineReadResults = runParallelReads(
    mongos.host,
    dbName,
    collNames,
    kReadersPerCollection,
    kBaselineDocCount,
);
const maxBaselineLatencyMillis = Math.max(...baselineReadResults.map((r) => r.latencyMillis));
jsTest.log.info(
    `Baseline parallel-read latency (warm CSRs): slowest single read ` +
        `${maxBaselineLatencyMillis}ms across ${baselineReadResults.length} readers.`,
);

// Snapshot the CSR disk-recovery diagnostics on the node that will roll back.
const rollbackNode = st.configRS.getPrimary();
function csrMetadataStats(node) {
    const ss = assert.commandWorked(node.adminCommand({serverStatus: 1}));
    return ss.shardingStatistics.collectionShardingMetadataStatistics;
}
const baselineStats = csrMetadataStats(rollbackNode);
jsTest.log.info(`Baseline collectionShardingMetadataStatistics: ${tojson(baselineStats)}`);

// In config-shard mode the config replica set is auto-bootstrapped, which bypasses the settings
// passed via configReplSetTestOptions. Reconfigure it explicitly to satisfy RollbackTest's
// preconditions: chaining disabled (so the tiebreaker can't be a sync source) and a "forever"
// election timeout (to avoid unplanned elections).
{
    const cfg = st.configRS.getReplSetConfigFromNode();
    cfg.version += 1;
    cfg.settings = cfg.settings || {};
    cfg.settings.chainingAllowed = false;
    cfg.settings.electionTimeoutMillis = ReplSetTest.kForeverMillis;
    // RollbackTest requires the primary to be members[0] with the highest priority, exactly one
    // priority:0 tiebreaker among the secondaries, and the two secondaries to differ in priority.
    const primaryIdx = cfg.members.findIndex((m) => m.host === rollbackNode.host);
    assert.gte(primaryIdx, 0, "could not find the current primary in the config");
    let assignedTiebreaker = false;
    cfg.members.forEach((m, i) => {
        if (i === primaryIdx) {
            m.priority = 2;
        } else if (!assignedTiebreaker) {
            m.priority = 0; // tiebreaker
            assignedTiebreaker = true;
        } else {
            m.priority = 1;
        }
    });
    // Ensure the primary occupies members[0] as RollbackTest asserts.
    if (primaryIdx !== 0) {
        const [primaryMember] = cfg.members.splice(primaryIdx, 1);
        cfg.members.unshift(primaryMember);
    }
    assert.commandWorked(rollbackNode.adminCommand({replSetReconfig: cfg}));
}

// --- Step 2: perform a live rollback, and force reset all CSRs.
const rbt = new RollbackTest(jsTestName(), st.configRS);
assert.eq(
    rollbackNode.host,
    rbt.getPrimary().host,
    "expected the pre-rollback primary to be the rollback node",
);

// Ensure that we capture crucial logs at a low verbosity.
assert.commandWorked(
    rollbackNode.adminCommand({setParameter: 1, logComponentVerbosity: {sharding: {verbosity: 0}}}),
);

// Force ShardingRecoveryService::onReplicationRollback to always reset all in-memory sharding
// states, regardless of which namespaces the rolled-back ops happened to touch.
assert.commandWorked(
    rollbackNode.adminCommand({configureFailPoint: forceResetFailPointName, mode: "alwaysOn"}),
);

// The log IDs we expect this test to generate.
const kResetLogIds = [
    10371108, // "Resetting all in-memory sharding states"
    10371109, // "Reset all in-memory sharding states"
];
const kRefreshLogIds = [
    12195302, // Reading collection sharding metadata from disk
    12033902, // Disk contents have been read
    12033903, // Wait for stable timestamp finished, proceeding to read disk contents
    12307913, // authoritative recovery completed (versionMatchedAfterDiskRecovery)
];
const kDiagnosticLogIds = [...kResetLogIds, ...kRefreshLogIds];
const diagnosticLogLinesPresent = () => {
    const global = checkLog.getGlobalLog(rollbackNode);
    return global.filter((line) => kDiagnosticLogIds.some((id) => line.includes(`"id":${id},`)));
};

// Clear the log and ensure that we don't already have these log IDs recorded.
assert.commandWorked(rollbackNode.adminCommand({clearLog: "global"}));
assert.eq(
    [],
    diagnosticLogLinesPresent(),
    "expected no diagnostic sharding log lines immediately after clearing the global log",
);

rbt.transitionToRollbackOperations();
// A throwaway write that will be rolled back; its content is irrelevant to the reset (forced).
assert.commandWorked(rbt.getPrimary().getDB(dbName)[collNames[0]].insert({_id: 12345, x: 1}));

rbt.transitionToSyncSourceOperationsBeforeRollback();
rbt.transitionToSyncSourceOperationsDuringRollback();
rbt.transitionToSteadyStateOperations();

assert.commandWorked(
    rollbackNode.adminCommand({configureFailPoint: forceResetFailPointName, mode: "off"}),
);

// The reset ran on the rolled-back node while it recovered. Restore it as primary so that the
// resumed CRUD from mongos is routed to the node whose CSR was cleared.
rbt.stepUpNode(rollbackNode);
assert.soon(
    () => st.configRS.getPrimary().host === rollbackNode.host,
    "rolled-back node did not become primary again",
);

// Let things settle after the rollback.
rbt.awaitLastOpCommitted(60 * 1000);

// Assert the reset actually happened on the rolled-back node and confirm we log the reset.
for (const id of kResetLogIds) {
    checkLog.containsJson(rollbackNode, id, {resetCollectionShardingRuntimes: true});
}

// Simulate the I/O cost of scanning a large chunk table during the CSR disk recovery: sleep this
// long per recovered chunk.
const kSleepMillisPerChunk = 25;
assert.commandWorked(
    rollbackNode.adminCommand({
        configureFailPoint: sleepPerChunkFailPointName,
        mode: "alwaysOn",
        data: {sleepMillisPerChunk: kSleepMillisPerChunk},
    }),
);

// --- Step 3: resume client traffic and measure the customer-visible latency. We spawn
// 'kReadersPerCollection' concurrent readers per collection, all routed to the rolled-back (now
// primary) node whose CSRs were cleared. Per collection, one reader triggers the delayed disk
// recovery while the other blocks on that same in-flight rebuild -- so both carry the wait.
const resumedReadResults = runParallelReads(
    mongos.host,
    dbName,
    collNames,
    kReadersPerCollection,
    kBaselineDocCount,
);
const maxClientLatencyMillis = Math.max(...resumedReadResults.map((r) => r.latencyMillis));
// Group per-thread latencies by collection so the log shows each collection's pair of readers.
const clientLatencyByColl = {};
for (const {collName, latencyMillis} of resumedReadResults) {
    (clientLatencyByColl[collName] = clientLatencyByColl[collName] || []).push(latencyMillis);
}
jsTest.log.info(
    `Client-observed resumed-read latency per collection (ms), ${kReadersPerCollection} ` +
        `concurrent readers each: ${tojson(clientLatencyByColl)}; slowest single read ` +
        `${maxClientLatencyMillis}ms (baseline warm-CSR slowest was ` +
        `${maxBaselineLatencyMillis}ms).`,
);

assert.commandWorked(
    rollbackNode.adminCommand({configureFailPoint: sleepPerChunkFailPointName, mode: "off"}),
);

// Diagnostic surface #1: After the reset, the resumed ops drive shard filtering metadata refreshes,
// which the node logs.
let refreshLogLines = [];
assert.soon(
    () => {
        const global = checkLog.getGlobalLog(rollbackNode);
        refreshLogLines = global.filter((line) =>
            kRefreshLogIds.some((id) => line.includes(`"id":${id},`)),
        );
        return refreshLogLines.length > 0;
    },
    "expected a shard filtering metadata refresh to be logged after the CSR reset",
    60 * 1000,
    undefined,
    {runHangAnalyzer: false},
);
jsTest.log.info(`Refresh log lines observed after reset: ${tojson(refreshLogLines)}`);

// Diagnostic surface #2: the shard-side CSR recovery counters under
// shardingStatistics.collectionShardingMetadataStatistics.
const afterStats = csrMetadataStats(rollbackNode);
jsTest.log.info(`Post-rollback collectionShardingMetadataStatistics: ${tojson(afterStats)}`);

// Quantify the whole story from serverStatus deltas:
//   collectionsRecovered = how many collections rebuilt their CSR from disk after the reset.
//   recovererCalls       = how many recovery rounds ran (>= collections; retries add more).
//   chunksRead           = total chunks read from the on-disk shard catalog across all recoveries.
//   diskRecoveryMillis   = accumulated wall-clock time spent reading that metadata from disk.
const collectionsRecovered =
    afterStats.countDiskRecoveriesPerformed - baselineStats.countDiskRecoveriesPerformed;
const recovererCalls =
    afterStats.countMetadataSynchronizersCreated - baselineStats.countMetadataSynchronizersCreated;
const chunksRead =
    afterStats.totalDiskRecoveryChunksRead - baselineStats.totalDiskRecoveryChunksRead;
const diskRecoveryMillis =
    afterStats.totalDiskRecoveryMillis - baselineStats.totalDiskRecoveryMillis;

jsTest.log.info(
    `CSR reset recovery story: ${collectionsRecovered} collection(s) rebuilt their CSR ` +
        `from disk over ${chunksRead} chunk(s), via ${recovererCalls} disk-recovery call(s), ` +
        `taking ${diskRecoveryMillis}ms accumulated across the delayed CRUD operations.`,
);

// Every collection whose CSR was cleared must have been rebuilt from disk.
assert.gte(
    collectionsRecovered,
    collNames.length,
    "expected every reset collection's CSR to be rebuilt from disk",
);
assert.gte(
    recovererCalls,
    collNames.length,
    "expected at least one disk-recovery call per reset collection",
);
// Each collection reads back all of its chunks, so the total chunks read must cover them all.
assert.gte(
    chunksRead,
    collNames.length * kChunksPerCollection,
    "expected the disk recoveries to read back all chunks of the reset collections",
);

// Diagnostic surface #3: tally the disk-read time spent rebuilding the CSRs against the delay we
// purposefully injected. Each recovery slept 'kChunksPerCollection * kSleepMillisPerChunk', so the
// accumulated totalDiskRecoveryMillis must cover the total injected delay across all collections.
const kExpectedDelayMillisPerCollection = kChunksPerCollection * kSleepMillisPerChunk;
const kExpectedTotalDelayMillis = kExpectedDelayMillisPerCollection * collNames.length;

// The observed latency need only reach this fraction of the fully-injected delay. We tolerate a
// shortfall because of timing jitter between the reads and the injected per-chunk sleep, and
// because when multiple threads recover the same CSR from disk concurrently, only one performs the
// full disk read while the others wait on the in-flight rebuild and can observe less than the full
// disk-read latency.
const kMinObservedDelayFraction = 0.5;

jsTest.log.info(
    `CSR reset latency tally: serverStatus reports ${diskRecoveryMillis}ms of disk-recovery ` +
        `time accumulated across the reset collections vs. ${kExpectedTotalDelayMillis}ms of ` +
        `purposefully injected CSR-rebuild delay.`,
);
assert.gte(
    diskRecoveryMillis,
    kExpectedTotalDelayMillis * kMinObservedDelayFraction,
    "accumulated disk-recovery time did not account for the injected CSR-rebuild delay",
);

// Diagnostic surface #4: the customer-visible latency. The resumed reads, that forced the CSRs to
// be refreshed, must experience materially elevated latency. With two concurrent readers per
// collection, one triggers the rebuild and the other waits on the same in-flight recovery, so both
// carry the wait; we assert on the slowest single read against a full collection's injected delay.
jsTest.log.info(
    `Customer-visible resumed-read latency: slowest single read ${maxClientLatencyMillis}ms; ` +
        `injected ${kExpectedDelayMillisPerCollection}ms per collection.`,
);
assert.gte(
    maxClientLatencyMillis,
    kExpectedDelayMillisPerCollection * kMinObservedDelayFraction,
    "no resumed read reflected the CSR-rebuild latency the customer would experience",
);

// RollbackTest leaves replication stopped on the tiebreaker; restart it so the config replica set
// (owned by ShardingTest) can be torn down cleanly. Do not call rbt.stop() here: ShardingTest owns
// this replica set and stops it itself.
restartReplSetReplication(st.configRS);
st.stop();
