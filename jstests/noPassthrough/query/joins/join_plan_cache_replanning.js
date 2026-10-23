/**
 * End to end tests for the main components of the join replanning process: recording a baseline
 * from a plan's first execution, deciding from that baseline whether the plan is eligible to be
 * cached, and checking a plan recovered from the cache against its baseline before serving it.
 *
 * @tags: [
 *   requires_fcv_91,
 *   requires_sbe,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {assertAllJoinsUseMethod} from "jstests/libs/query/join_utils.js";
import {runWithKnobs} from "jstests/libs/query/knob_utils.js";

// Shared by every describe below: one mongod, the base/foreign collections, and the join pipeline.
before(function () {
    this.conn = MongoRunner.runMongod({
        setParameter: {
            internalEnableJoinOptimization: true,
            internalEnableJoinPlanCache: true,
        },
    });

    const db = this.conn.getDB(jsTestName());
    this.db = db;
    this.baseColl = db[jsTestName()];
    this.foreignColl = db[jsTestName() + "_a"];

    const baseDocs = [];
    const foreignDocs = [];
    for (let i = 0; i < 100; ++i) {
        baseDocs.push({a: i, b: i % 2});
        foreignDocs.push({b: i % 2, c: "c" + i});
    }
    assert.commandWorked(this.baseColl.insertMany(baseDocs));
    assert.commandWorked(this.foreignColl.insertMany(foreignDocs));
    assert.commandWorked(this.baseColl.createIndex({dummy: 1, a: 1, b: 1}));
    assert.commandWorked(this.foreignColl.createIndex({dummy: 1, b: 1, c: 1}));

    this.pipeline = [
        {$match: {a: {$gt: 90}}},
        {
            $lookup: {
                from: this.foreignColl.getName(),
                localField: "b",
                foreignField: "b",
                as: "foreignColl",
            },
        },
        {$unwind: "$foreignColl"},
    ];
});

after(function () {
    MongoRunner.stopMongod(this.conn);
});

beforeEach(function () {
    assert.commandWorked(this.db.adminCommand({clearJoinPlanCache: 1}));
});

function entries(db) {
    return db
        .getSiblingDB("admin")
        .aggregate([{$joinPlanCacheStats: {}}])
        .toArray();
}

function joinMetrics(db) {
    return db.serverStatus().metrics.query.planCache.join;
}

describe("baseline and cache eligibility", function () {
    it("records a baseline and correctly reports a baseline for cache-eligible queries", function () {
        const cursor = this.baseColl.aggregate(this.pipeline);
        assert.eq(450, cursor.toArray().length);

        const stats = entries(this.db);
        assert.eq(1, stats.length, "expected exactly one cache entry", {stats});
        const baseline = stats[0].baseline;
        assert(baseline !== undefined, "cache entry missing baseline", {stats});
        assert.gt(baseline.numReads + baseline.numIntermediateRows, 0, {baseline});
        assert.eq(baseline.spilled, false, {baseline});

        // baselineOverThreshold metric should not increment.
        const baselineOverThresholdBefore = joinMetrics(this.db).baselineOverThreshold;
        assert.eq(joinMetrics(this.db).baselineOverThreshold, baselineOverThresholdBefore, {
            baselineOverThresholdBefore,
        });
    });

    it("records a baseline independently of the request's batch size", function () {
        // The baseline is recorded from a fixed window of documents, unaffected by the request's
        // batch size: batchSize: 0 gives the client an empty first batch, but the baseline run
        // still executes the full window.
        const cursor = this.baseColl.aggregate(this.pipeline, {cursor: {batchSize: 0}});
        assert.eq(450, cursor.toArray().length);

        const stats = entries(this.db);
        assert.eq(1, stats.length, "expected exactly one cache entry", {stats});
        const baseline = stats[0].baseline;
        assert(baseline !== undefined, "cache entry missing baseline", {stats});
        assert.gt(baseline.numReads + baseline.numIntermediateRows, 0, {baseline});
        assert.eq(baseline.spilled, false, {baseline});
    });

    it("does not cache a plan whose baseline exceeds the gate", function () {
        runWithKnobs(
            this.db,
            () => {
                const baselineOverThresholdBefore = joinMetrics(this.db).baselineOverThreshold;
                assert.eq(450, this.baseColl.aggregate(this.pipeline).toArray().length);
                assert.eq(0, entries(this.db).length, "expected no cache entry");
                assert.gt(joinMetrics(this.db).baselineOverThreshold, baselineOverThresholdBefore, {
                    baselineOverThresholdBefore,
                });
            },
            // Set small threshold so the baseline trips the cache-eligibility check.
            {internalQueryJoinPlanCacheMaxReadsAndIntermediateRows: 1},
        );
    });

    it("does not cache a plan whose baseline trial hits the stash byte cap", function () {
        runWithKnobs(
            this.db,
            () => {
                const before = joinMetrics(this.db).baselineOverStashMemoryLimit;
                assert.eq(450, this.baseColl.aggregate(this.pipeline).toArray().length);
                assert.eq(0, entries(this.db).length, "expected no cache entry");
                assert.eq(joinMetrics(this.db).baselineOverStashMemoryLimit, before + 1, {before});
            },
            // Small enough that the first few results fill the stash.
            {internalQuerySBEPlanEvaluationMaxMemoryBytes: 1024},
        );
    });

    it("records a spilled baseline when the join spills to disk", function () {
        runWithKnobs(
            this.db,
            () => {
                // Force a hash join: only HJ spills. The 1-byte threshold makes the first build
                // row exceed the limit and spill.
                assertAllJoinsUseMethod(this.baseColl.explain().aggregate(this.pipeline), "HJ");

                assert.eq(450, this.baseColl.aggregate(this.pipeline).toArray().length);

                const stats = entries(this.db);
                assert.eq(1, stats.length, "expected exactly one cache entry", {stats});
                assert.eq(stats[0].baseline.spilled, true, "expected a spilled baseline", {stats});
            },
            {
                internalJoinMethod: "HJ",
                internalQuerySlotBasedExecutionHashJoinApproxMemoryUseInBytesBeforeSpill: 1,
            },
        );
    });

    it("fails the query instead of returning partial results when the trial fails due to disallowed disk use", function () {
        runWithKnobs(
            this.db,
            () => {
                // A hash join that must spill errors out when disk use is disallowed.
                assertAllJoinsUseMethod(this.baseColl.explain().aggregate(this.pipeline), "HJ");
                assert.throwsWithCode(
                    () => this.baseColl.aggregate(this.pipeline, {allowDiskUse: false}).toArray(),
                    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
                );
                assert.eq(0, entries(this.db).length, "expected no cache entry");
            },
            {
                internalJoinMethod: "HJ",
                internalQuerySlotBasedExecutionHashJoinApproxMemoryUseInBytesBeforeSpill: 1,
            },
        );
    });

    it("reports usedJoinOptimization for a plan that ran a baseline trial", function () {
        const comment = "baseline_used_join_optimization";
        const previousProfilingLevel = assert.commandWorked(this.db.setProfilingLevel(2)).was;
        try {
            assert.eq(450, this.baseColl.aggregate(this.pipeline, {comment}).toArray().length);
        } finally {
            assert.commandWorked(this.db.setProfilingLevel(previousProfilingLevel));
        }
        assert.eq(1, entries(this.db).length, "expected the plan to be cached");

        const entry = getLatestProfilerEntry(this.db, {"command.comment": comment});
        assert.eq(true, entry.usedJoinOptimization, {entry});
    });

    it("returns complete results when the result set is larger than the trial window", function () {
        // Small batches make the client pull results both from the trial and from the plan.
        const actual = this.baseColl.aggregate(this.pipeline, {cursor: {batchSize: 7}}).toArray();
        assert.eq(1, entries(this.db).length, "expected the plan to be cached");

        const expected = runWithKnobs(
            this.db,
            () => this.baseColl.aggregate(this.pipeline).toArray(),
            {internalEnableJoinPlanCache: false},
        );
        assert.gt(expected.length, 101, "result set must exceed the trial window");
        assertArrayEq({actual, expected});
    });
});
