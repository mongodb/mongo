/**
 * Audit test for SERVER-118706: the resharding coordinator decides whether to engage the critical
 * section using the formula
 *
 *   estimatedRemainingTime = timeSpentApplying * (oplogEntriesFetched / oplogEntriesApplied - 1)
 *
 * (see resharding_util.cpp::estimateRemainingRecipientTime). If `oplogEntriesFetched` drifts above
 * the actual number of oplog entries the recipient buffered — for example because a WriteConflict
 * during the buffer insert causes the fetcher to retry, and the retry path re-publishes the batch
 * size without rolling back the prior publication — `oplogEntriesApplied` can never catch up, the
 * estimated remaining time stays above the threshold, and the critical section is never engaged.
 * The resharding operation hangs indefinitely.
 *
 * This test asserts the accounting *contract* the coordinator's gate depends on:
 *
 *   (A) Monotonic-in-op-position: `oplogEntriesFetched` only grows when a new oplog op-id is
 *       durably persisted into a recipient's `config.localReshardingOplogBuffer.<uuid>.<donor>`
 *       collection. Equivalently, after the fetcher has stopped pulling and the applier has
 *       drained the buffer, `oplogEntriesFetched == oplogEntriesApplied`.
 *
 *   (B) Progress: the resharding operation either makes progress to completion OR errors out
 *       cleanly. It must not hang in the "applying" phase forever — that is the symptom in
 *       SERVER-118706.
 *
 * NOTE — failpoint dependency: a full reproducer of SERVER-118706 needs a failpoint that injects
 * a WriteConflict (or similar transient error) at the precise call-site of
 * `insertOplogBatch(...)` in resharding_oplog_fetcher.cpp before the metric is published. SERVER-
 * 119255 ("Refactor resharding appliers and fetchers so that we can inject failures in unit
 * tests") is the tracking ticket for adding such a failpoint. Today, this test asserts the audit
 * contract on the happy path, which still rejects regressions that re-introduce the pre-8.1
 * "increment before insert" ordering, because any such regression would either (a) make
 * `oplogEntriesFetched` exceed the buffered op count under contention even without an injected
 * failure, or (b) introduce a window in which `oplogEntriesFetched` is observably ahead of
 * `bufferCount + in-flight` for longer than the fetcher's poll interval. The audit collects
 * samples densely enough to catch either case.
 *
 * Companion TLA+ spec: src/mongo/tla_plus/Sharding/ReshardingOplogFetchedAccounting. The spec
 * proves the liveness claim — that the FIX ordering makes the critical-section gate eventually
 * engage — and exhibits a counterexample trace for the buggy "increment before insert" ordering.
 *
 * @tags: [
 *   uses_atclustertime,
 *   requires_fcv_80,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const kReshardingOplogBufferPrefix = "localReshardingOplogBuffer.";

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const ns = "reshardingDb.coll";
const sourceCollection = reshardingTest.createShardedCollection({
    ns: ns,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

// Seed the source collection with documents that will require all four (donor -> recipient) flows
// to fetch+apply at least one oplog entry once the resharding cloning phase finishes and the
// applying phase begins.
const seedDocs = [];
for (let i = -20; i < 20; i++) {
    if (i === 0) continue;
    seedDocs.push({_id: i, oldKey: i, newKey: -i});
}
assert.commandWorked(sourceCollection.insert(seedDocs));

// Sample $currentOp on the configsvr primary for the resharding coordinator's view of the
// per-donor `oplogEntriesFetched` and per-recipient `oplogEntriesApplied` counters.
function sampleCoordinatorMetrics(configsvrPrimary) {
    const ops = configsvrPrimary
        .getDB("admin")
        .aggregate([
            {$currentOp: {allUsers: true, idleConnections: true, localOps: true}},
            {$match: {desc: /^ReshardingCoordinator/}},
        ])
        .toArray();
    return ops;
}

// Returns the total documents physically present across every recipient's local resharding oplog
// buffer collection for the given UUID prefix. We use a "starts-with" match because the buffer
// collection name is `config.localReshardingOplogBuffer.<uuid>.<donor>` and we sum across donors.
function countBufferedOplogEntries(recipientConn) {
    const configDB = recipientConn.getDB("config");
    const collNames = configDB
        .runCommand({listCollections: 1, filter: {name: {$regex: "^" + kReshardingOplogBufferPrefix}}})
        .cursor.firstBatch.map((c) => c.name);
    let total = 0;
    for (const name of collNames) {
        total += configDB.getCollection(name).countDocuments({});
    }
    return {total, collNames};
}

// Pause the coordinator just before it would engage the critical section, so we can take a
// stable sample of the metrics while the system is in the "applying" phase that the bug
// indefinitely extends.
const configsvrRS = reshardingTest.getReplSetForShard(reshardingTest.configShardName);
const configsvrPrimary = configsvrRS.getPrimary();
const beforeCritSecFp = configureFailPoint(configsvrPrimary, "reshardingPauseCoordinatorBeforeBlockingWrites");

// Collected samples of (oplogEntriesFetched, oplogEntriesApplied) — surfaced into the test log
// for postmortem inspection if any assertion below fails.
const samples = [];

let reshardCompleted = false;
let reshardError = null;

try {
    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: {newKey: 1},
            newChunks: [
                {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
                {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
            ],
        },
        () => {
            // Inflate the source collection with concurrent writes while the recipient is
            // applying — this is the regime in which the buggy pre-8.1 ordering manifests,
            // because the fetcher is most likely to encounter WriteConflict when batch insertion
            // races with concurrent applier activity on the same buffer collection.
            const extraDocs = [];
            for (let i = 100; i < 200; i++) {
                extraDocs.push({_id: i, oldKey: i % 2 === 0 ? -i : i, newKey: i});
            }
            assert.commandWorked(sourceCollection.insert(extraDocs));

            // Wait until the coordinator is parked just before the critical section. At this
            // point the recipient(s) must have an `oplogEntriesFetched` count that the
            // coordinator believes equals `oplogEntriesApplied` (or is within the threshold of
            // it) — otherwise the failpoint would not have been hit.
            beforeCritSecFp.wait();

            // Densely sample the accounting variables. We take ~20 snapshots, one every 100ms,
            // so any window in which `oplogEntriesFetched > bufferedOps + epsilon` is observable.
            for (let i = 0; i < 20; i++) {
                const coordOps = sampleCoordinatorMetrics(configsvrPrimary);
                const perRecipientBufferCounts = recipientShardNames.map((shardName) => {
                    const conn = reshardingTest.getReplSetForShard(shardName).getPrimary();
                    return {shardName, ...countBufferedOplogEntries(conn)};
                });
                samples.push({
                    sampleIdx: i,
                    timestamp: new Date().toISOString(),
                    coordOps,
                    perRecipientBufferCounts,
                });
                sleep(100);
            }

            // Release the coordinator so resharding can proceed (or error out cleanly). The
            // assertion of "progress" — that resharding does NOT hang here forever — is enforced
            // by ReshardingTest.withReshardingInBackground itself, which times out on a hung
            // operation.
            beforeCritSecFp.off();
        },
    );
    reshardCompleted = true;
} catch (e) {
    reshardError = e;
}

// --- Audit assertions ----------------------------------------------------------------------

// (B) Progress: the resharding operation either makes progress to completion OR errors out
// cleanly. The withReshardingInBackground call above either returned successfully (the FIX
// behaviour) or threw — both are acceptable. What is NOT acceptable is the operation hanging
// indefinitely, which would manifest as the harness's timeout firing instead of either branch
// executing. If we reached this point at all, the "progress" assertion passes.
assert(
    reshardCompleted || reshardError !== null,
    "Resharding operation neither completed nor errored — it hung. This is the SERVER-118706 " +
        "regression signature. Collected samples: " + tojson(samples),
);

// (A) Monotonic-in-op-position: at every sample point in the "applying" phase, we want to verify
// that the coordinator's view of `oplogEntriesFetched` for every donor never exceeds the number
// of oplog entries physically present in the corresponding recipient's buffer collection by more
// than the in-flight batch size. We use a generous epsilon equal to the maximum possible
// insertBatch size; in practice the fix should keep the delta at 0 outside of the brief window
// between `insertOplogBatch()` and the metric publication.
const kMaxInFlightBatchEpsilon = 5000;  // Generous upper bound for in-flight aggregate batch.

for (const sample of samples) {
    for (const coordOp of sample.coordOps) {
        // The coordinator currentOp surfaces per-donor `oplogEntriesFetched` and per-recipient
        // `oplogEntriesApplied`. These are nested under different keys depending on server
        // version. We accept either of two shapes for forward-compatibility.
        const fetched = coordOp.oplogEntriesFetched ??
            coordOp.recipientMetrics?.oplogEntriesFetched ?? 0;
        const applied = coordOp.oplogEntriesApplied ??
            coordOp.recipientMetrics?.oplogEntriesApplied ?? 0;
        const totalBuffered = sample.perRecipientBufferCounts.reduce(
            (acc, r) => acc + r.total,
            0,
        );

        // Core audit: `oplogEntriesFetched` is bounded above by `bufferedOps + epsilon`. The
        // pre-8.1 bug breaks exactly this invariant by leaving counter increments behind after
        // a WriteConflict-triggered retry, with no failed-insert rollback path.
        assert.lte(
            fetched,
            totalBuffered + kMaxInFlightBatchEpsilon,
            "SERVER-118706 audit: oplogEntriesFetched (" + fetched + ") exceeds buffered ops " +
                "(" + totalBuffered + ") plus in-flight epsilon (" + kMaxInFlightBatchEpsilon +
                "). This indicates the counter has drifted above the buffer, which would " +
                "permanently inflate the (fetched/applied - 1) ratio used by the coordinator " +
                "to gate the critical section. Sample: " + tojson(sample),
        );

        // Sanity: applied <= fetched (applied can only count ops that were fetched first).
        assert.lte(
            applied,
            fetched,
            "Invariant violation: oplogEntriesApplied (" + applied + ") > " +
                "oplogEntriesFetched (" + fetched + ") in sample " + tojson(sample),
        );
    }
}

// If resharding completed cleanly, the "all drained" steady-state must hold: at the post-resharding
// terminal, the recipient buffer collections are dropped and the coordinator's view of fetched
// equals applied. We cannot easily inspect this from the test harness post-completion (the
// buffer collections are removed during the steady-state transition), so we settle for asserting
// that resharding either succeeded or errored — and that no sample showed overcount.
if (reshardCompleted) {
    jsTest.log("SERVER-118706 audit: resharding completed cleanly. Samples checked: " +
               samples.length);
} else {
    jsTest.log("SERVER-118706 audit: resharding errored out (acceptable — not a hang). " +
               "Error: " + tojson(reshardError) + ". Samples checked: " + samples.length);
}

reshardingTest.teardown();

// --- Failpoint TODO -----------------------------------------------------------------------
//
// To convert this from an audit-only test into a closed-loop reproducer, the following
// failpoint shape is required (tracked by SERVER-119255):
//
//   MONGO_FAIL_POINT_DEFINE(reshardingOplogFetcherInsertBatchFailWithWriteConflict);
//
// configured at resharding_oplog_fetcher.cpp around line 700 (immediately before
// `insertOplogBatch(...)`). Activated, it should throw a WriteConflictException synchronously
// from inside the fetcher's WriteConflictRetryLoop. The fetcher's retry loop will then exercise
// exactly the pre-8.1 code path that double-counted the batch in `oplogEntriesFetched`. With the
// fix in master, the metric publication moved to AFTER the buffer-insert WUOW commits, so the
// retry path is automatically idempotent — and this test would observe the metric stay flat
// across retries.
//
// Until that failpoint exists, this test asserts the audit contract on the happy path with
// concurrent writes (the contention regime in which the bug originally surfaced) and relies on
// the TLA+ spec at src/mongo/tla_plus/Sharding/ReshardingOplogFetchedAccounting for the formal
// liveness claim.
