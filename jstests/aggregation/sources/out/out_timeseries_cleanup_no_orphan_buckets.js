/**
 * Regression test for SERVER-94828.
 *
 * When $out targets a time-series collection, the cleanup path (run from OutStage::doDispose())
 * is responsible for dropping the temporary system.buckets collection if the pipeline is
 * interrupted or fails after the temp-rename but before the user-facing view is created.
 *
 * The bug: cleanup drops by namespace name, not by UUID. If a concurrent operation drops and
 * re-creates a buckets collection between rename and cleanup, the cleanup path could either:
 *   (a) drop an unrelated collection (data loss), or
 *   (b) leave orphan system.buckets.tmp.agg_out.<UUID> collections behind.
 *
 * Stressing the cleanup path while a concurrent $out is in flight exercises both directions
 * of the race. This regression test asserts that after the dust settles, the only buckets
 * collection present is the legitimately-owned target (or nothing at all), with no leaked
 * system.buckets.tmp.agg_out.* namespaces.
 *
 * @tags: [
 *   # We need a time-series collection.
 *   requires_timeseries,
 *   # Standalone-style test: do not run under sharded / multi-version passthroughs.
 *   assumes_unsharded_collection,
 *   assumes_against_mongod_not_mongos,
 *   # The second concurrent $out is expected to fail-fast and the cleanup races; do not retry
 *   # the pipeline under suites that re-run on transient errors.
 *   does_not_support_transactions,
 *   does_not_support_stepdowns,
 *   # Touches non-namespace-quoted listCollections filters; skip on multitenancy.
 *   simulate_atlas_proxy_incompatible,
 * ]
 */
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const testDB = db.getSiblingDB(jsTestName());
const kSource = "src";
const kTarget = "tgt_ts";

assert.commandWorked(testDB.dropDatabase());

// Seed source collection. Documents must carry a valid time field so that they're acceptable
// to a time-series $out.
const sourceColl = testDB[kSource];
const seed = [];
const baseTime = ISODate("2024-01-01T00:00:00Z");
for (let i = 0; i < 200; i++) {
    seed.push({_id: i, t: new Date(baseTime.getTime() + i * 1000), x: i});
}
assert.commandWorked(sourceColl.insert(seed));

function listAggTempBuckets() {
    // Match the canonical temp namespace prefix used by $out: tmp.agg_out.<uuid>. When the
    // target is time-series, the underlying buckets collection is system.buckets.tmp.agg_out.*.
    const all = testDB.getCollectionNames();
    return all.filter((c) => /^(system\.buckets\.)?tmp\.agg_out\./.test(c));
}

function runOutToTimeseries(comment) {
    // Under contention one of the two $outs may fail-fast. We don't care which one wins —
    // we only care that neither leaves orphan temp buckets behind.
    return testDB.runCommand({
        aggregate: kSource,
        pipeline: [{$out: {db: testDB.getName(), coll: kTarget, timeseries: {timeField: "t"}}}],
        cursor: {},
        comment: comment,
    });
}

// Baseline sanity: no leftover temp namespaces before we start.
assert.eq([], listAggTempBuckets(), "test started with stale tmp.agg_out collections");

// Drive the cleanup-path race. We fire one $out in the foreground and a second $out from a
// parallel shell aimed at the same target. The second one will typically fail-fast (the
// first $out is mid-rename), but doing so stresses the cleanup destructor while there is
// concurrent buckets-namespace churn — which is exactly the window the bug opens.
const kCommentA = "out_ts_cleanup_race_A";
const kCommentB = "out_ts_cleanup_race_B";

const awaitParallel = startParallelShell(
    funWithArgs(
        function (dbName, src, tgt, comment) {
            const d = db.getSiblingDB(dbName);
            // Best-effort: we don't assert on the outcome of this $out — it may succeed or
            // fail depending on how the kernel interleaves rename / cleanup. The purpose is
            // to keep concurrent activity on the buckets namespace during the foreground
            // $out's cleanup window.
            d.runCommand({
                aggregate: src,
                pipeline: [{$out: {db: dbName, coll: tgt, timeseries: {timeField: "t"}}}],
                cursor: {},
                comment: comment,
            });
        },
        testDB.getName(),
        kSource,
        kTarget,
        kCommentB,
    ),
    db.getMongo().port,
);

// Run several iterations of the foreground $out. Each iteration drops + recreates the
// buckets namespace and so widens the window in which the cleanup path could grab the
// wrong collection by name.
const kIterations = 8;
for (let i = 0; i < kIterations; i++) {
    const res = runOutToTimeseries(kCommentA + "_" + i);
    // We tolerate either success or a NamespaceExists / Interrupted / similar fail-fast.
    // The bug is in cleanup, not in fail-fast behaviour.
    if (!res.ok) {
        // If it failed, ensure it failed for a known concurrent-DDL reason rather than
        // crashing the server or wedging the pipeline.
        const allowedCodes = [
            ErrorCodes.NamespaceExists,
            ErrorCodes.Interrupted,
            ErrorCodes.InterruptedAtShutdown,
            ErrorCodes.InterruptedDueToReplStateChange,
            ErrorCodes.ConflictingOperationInProgress,
            ErrorCodes.CollectionUUIDMismatch,
            ErrorCodes.NamespaceNotFound,
        ];
        assert.contains(
            res.code,
            allowedCodes,
            "foreground $out failed with unexpected code " + tojson(res),
        );
    }
}

awaitParallel();

// SERVER-94828 is specifically about orphan system.buckets.tmp.agg_out.<uuid> namespaces —
// those should NEVER survive a $out, regardless of whether the pipeline succeeded or was
// interrupted. A surviving kTarget is fine; a surviving tmp.agg_out.* is the bug, so we
// intentionally check temp namespaces without first dropping the target.
const orphans = listAggTempBuckets();
assert.eq(
    [],
    orphans,
    "SERVER-94828 regression: $out left orphan temp bucket collections after cleanup race: " +
        tojson(orphans),
);

// Clean up. The target may or may not exist depending on which iteration "won"; either
// outcome is acceptable for cleanup, so we don't assert.
testDB[kTarget].drop();
testDB[kSource].drop();
