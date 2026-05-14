/**
 * SERVER-119312: pin the corrected query stats metric semantics for writes on the router.
 *
 * Today, on the router, `lastExecutionMicros` and `totalExecMicros` are emitted for write
 * shapes as the *batch* duration (the router does not yet time individual ops in a batched
 * write) with pauses subtracted (via `elapsedTimeExcludingPauses`). Both choices are
 * misleading for write SLOs: pause-subtraction makes the number neither the user-perceived
 * latency nor the per-shard execution time, and per-batch stamping inflates `totalExecMicros.sum`
 * roughly linearly in batch size.
 *
 * This jstest pins three invariants of the redefined semantics described in
 * `src/mongo/db/query/query_stats/SERVER-119312-DESIGN.md`:
 *
 *   1. boundary tags `executionTimeBoundary` and `routerLatencySource` are present on
 *      router-side write shapes;
 *   2. per-shape `.max` wall latency >= per-shape `.max` exec time;
 *   3. when a single client batch contains multiple ops of the same shape, the wall-latency
 *      aggregation increments once per batch (not once per op).
 *
 * Until the implementation lands, the new fields are absent and the boundary-tag and
 * cardinality assertions skip themselves. The shape of the test stays stable so the
 * implementation diff is local. The "latency >= exec" invariant is asserted only when the
 * new field is present (otherwise there is no second number to compare against).
 *
 * @tags: [requires_fcv_90]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {getLatestQueryStatsEntry, resetQueryStatsStore} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// True when the SERVER-119312 redefinition is live and entries carry the boundary tag.
function hasRedefinition(entry) {
    return entry &&
        entry.metrics &&
        typeof entry.metrics.executionTimeBoundary === "string";
}

// Return the AggregatedMetric for router latency, or null if not yet emitted.
function routerLatencyAggregated(entry) {
    if (!entry || !entry.metrics) {
        return null;
    }
    return entry.metrics.routerLatencyMicros || null;
}

describe("SERVER-119312 query stats router write latency vs exec semantics", function () {
    let st;
    let mongos;
    let testDB;

    before(function () {
        const queryStatsParams = {
            internalQueryStatsWriteCmdSampleRate: 1,
        };
        st = new ShardingTest({
            shards: 2,
            mongosOptions: {setParameter: queryStatsParams},
            rsOptions: {setParameter: queryStatsParams},
        });
        mongos = st.s;
        testDB = mongos.getDB("test");
        assert.commandWorked(
            testDB.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}),
        );
    });

    after(function () {
        st?.stop();
    });

    beforeEach(function () {
        resetQueryStatsStore(mongos, "1MB");
    });

    describe("router-side write shape", function () {
        const collName = jsTestName() + "_router_write";
        let coll;

        before(function () {
            coll = testDB[collName];
            // Targeted shard-key writes: keeps the wall time predictable and avoids the
            // two-phase / WCOS / StaleConfig paths that have separate metric-propagation gaps.
            st.shardColl(coll, {_id: 1}, {_id: 0}, {_id: 0});
        });

        beforeEach(function () {
            assert.commandWorked(coll.deleteMany({}));
            assert.commandWorked(
                coll.insertMany([
                    {_id: -2, v: 1},
                    {_id: -1, v: 2},
                    {_id: 1, v: 3},
                    {_id: 2, v: 4},
                ]),
            );
        });

        // INVARIANT 1: boundary tags are present on router-side write shapes.
        //
        // Skips itself when the new fields are not yet emitted, so the test is harmless on
        // pre-redefinition builds. Becomes a hard assertion once
        // featureFlagQueryStatsRouterWriteLatency is on by default.
        it("should tag the boundary of lastExecutionMicros and routerLatencyMicros", function () {
            const result = assert.commandWorked(
                testDB.runCommand({
                    update: collName,
                    updates: [{q: {_id: 1}, u: {$set: {v: 100}}, multi: false}],
                    comment: "boundary tag check",
                }),
            );
            assert.eq(result.nModified, 1);

            const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "update");
            assert.gt(entry.metrics.lastExecutionMicros, 0);

            if (!hasRedefinition(entry)) {
                jsTestLog(
                    "SERVER-119312 redefinition not live on this build; skipping boundary-tag assertions. " +
                    "Update consumers when featureFlagQueryStatsRouterWriteLatency flips on.",
                );
                return;
            }

            // executionTimeBoundary documents the aggregation boundary of the legacy
            // lastExecutionMicros / totalExecMicros fields on this shape.
            assert.eq(
                "clientBatch",
                entry.metrics.executionTimeBoundary,
                "executionTimeBoundary should be 'clientBatch' until SERVER-121325 introduces " +
                    "per-op timing on the router for batched writes",
            );

            // routerLatencySource documents the boundary of the new routerLatencyMicros
            // aggregation: one sample per client batched command, not per op-index.
            assert.eq("clientBatch", entry.metrics.routerLatencySource);

            const routerLatency = routerLatencyAggregated(entry);
            assert.neq(null, routerLatency, "routerLatencyMicros should be present when redefinition is live");
            for (const field of ["sum", "max", "min", "sumOfSquares"]) {
                assert(routerLatency.hasOwnProperty(field), "routerLatencyMicros missing field " + field);
            }

            assert.gt(entry.metrics.lastRouterLatencyMicros, 0);
        });

        // INVARIANT 2: per-shape wall latency >= per-shape pause-subtracted exec time.
        //
        // Asserted only on `.max` (the only AggregatedMetric field that is meaningfully
        // comparable across shapes without a per-batch label channel). Skips when the new
        // field is absent.
        it("should report routerLatencyMicros >= totalExecMicros on .max", function () {
            // Drive a handful of identical-shape writes so AggregatedMetric.max stabilises.
            for (let i = 0; i < 5; i++) {
                assert.commandWorked(
                    testDB.runCommand({
                        update: collName,
                        updates: [{q: {_id: 1}, u: {$set: {v: 100 + i}}, multi: false}],
                        comment: "latency vs exec",
                    }),
                );
            }

            const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            assert.gte(entry.metrics.execCount, 5);

            const routerLatency = routerLatencyAggregated(entry);
            if (routerLatency === null) {
                jsTestLog(
                    "routerLatencyMicros not yet emitted on this build; skipping latency >= exec " +
                    "assertion (SERVER-119312 Phase 2 not yet landed).",
                );
                return;
            }

            const totalExec = entry.metrics.totalExecMicros;
            // Wall latency includes any pauses that elapsedTimeExcludingPauses subtracts, so
            // for any individual op latency >= exec; the same holds for the per-shape max.
            assert.gte(
                routerLatency.max,
                totalExec.max,
                "routerLatencyMicros.max should be >= totalExecMicros.max; got " +
                    tojson({routerLatency: routerLatency, totalExec: totalExec}),
            );
        });

        // INVARIANT 3: when a single client batch contains multiple ops of the same shape,
        // routerLatencyMicros accumulates once per batch while totalExecMicros / execCount
        // accumulate once per op. Pins the aggregation-boundary distinction.
        it("should aggregate routerLatencyMicros per batch, not per op", function () {
            assert.commandWorked(
                testDB.runCommand({
                    update: collName,
                    updates: [
                        {q: {_id: 1}, u: {$set: {v: 1001}}, multi: false},
                        {q: {_id: 2}, u: {$set: {v: 1002}}, multi: false},
                    ],
                    comment: "two-op batch",
                }),
            );

            const entryAfterOne = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            const execAfterOne = entryAfterOne.metrics.execCount;
            const routerLatencyAfterOne = routerLatencyAggregated(entryAfterOne);

            // Same-shape two-op batch a second time, so we have two batches' worth of samples.
            assert.commandWorked(
                testDB.runCommand({
                    update: collName,
                    updates: [
                        {q: {_id: 1}, u: {$set: {v: 2001}}, multi: false},
                        {q: {_id: 2}, u: {$set: {v: 2002}}, multi: false},
                    ],
                    comment: "two-op batch",
                }),
            );

            const entryAfterTwo = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            const execAfterTwo = entryAfterTwo.metrics.execCount;
            const routerLatencyAfterTwo = routerLatencyAggregated(entryAfterTwo);

            // execCount grows by 2 per batch (one per op of this shape).
            assert.eq(
                execAfterTwo - execAfterOne,
                2,
                "execCount should grow by ops-in-batch (2), got " + (execAfterTwo - execAfterOne),
            );

            if (routerLatencyAfterOne === null || routerLatencyAfterTwo === null) {
                jsTestLog(
                    "routerLatencyMicros not yet emitted; skipping per-batch aggregation assertion " +
                    "(SERVER-119312 Phase 2 not yet landed).",
                );
                return;
            }

            // The router-latency aggregation should grow by exactly one sample per batch.
            // AggregatedMetric does not expose a count directly, but `.sum` is the running
            // total of samples, so sum grows by exactly one batch-duration per batch — i.e.
            // (sum_after_two - sum_after_one) should be a single sample's value, plausibly
            // close to lastRouterLatencyMicros from the second batch. We assert the weaker
            // structural property: sum grew by a value that is consistent with one sample,
            // not two. Two-op batches that took T each would give sum-delta = T (good) under
            // per-batch aggregation, vs sum-delta = 2T under per-op aggregation.
            const sumDelta = routerLatencyAfterTwo.sum - routerLatencyAfterOne.sum;
            const lastSample = entryAfterTwo.metrics.lastRouterLatencyMicros;
            // sumDelta should equal exactly one sample (the second batch's latency),
            // which the runtime exposes as lastRouterLatencyMicros.
            assert.eq(
                NumberLong(sumDelta).toString(),
                NumberLong(lastSample).toString(),
                "routerLatencyMicros.sum should grow by exactly one sample per batch; got delta=" +
                    sumDelta +
                    ", lastSample=" +
                    lastSample,
            );
        });
    });
});
