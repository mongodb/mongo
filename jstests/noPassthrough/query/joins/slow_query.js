/**
 * Tests that the plan summary reported for a query answered by the join optimizer represents the join order and the join methods.
 * The summary is reported both by the profiler and by the "Slow query" log line, and this test asserts that the two agree for each executed plan below.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_profiling,
 *   requires_sbe
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {findMatchingLogLine} from "jstests/libs/log.js";

describe("plan summary for join plans", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({setParameter: {featureFlagPathArrayness: true}});
        const db = this.conn.getDB(jsTestName());
        this.db = db;

        this.coll = db[jsTestName()];
        this.foreign1 = db[jsTestName() + "_foreign1"];
        this.foreign2 = db[jsTestName() + "_foreign2"];

        assert.commandWorked(
            this.coll.insertMany([
                {_id: 0, a: 1, b: "foo"},
                {_id: 1, a: 2, b: "bar"},
                {_id: 2, a: 2, b: "foo"},
            ]),
        );
        assert.commandWorked(
            this.foreign1.insertMany([
                {_id: 0, a: 1, c: "x"},
                {_id: 1, a: 2, c: "y"},
            ]),
        );
        assert.commandWorked(
            this.foreign2.insertMany([
                {_id: 0, b: "foo", d: 10},
                {_id: 1, b: "bar", d: 20},
            ]),
        );

        // Add indexes for multikeyness info for path arrayness.
        assert.commandWorked(this.coll.createIndex({a: 1, b: 1}));
        // And a second index for the non-$join test.
        assert.commandWorked(this.coll.createIndex({a: 1}));
        assert.commandWorked(this.foreign1.createIndex({a: 1, c: 1}));
        assert.commandWorked(this.foreign2.createIndex({b: 1, d: 1}));

        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalEnableJoinOptimization: true,
                internalEnableJoinPlanCache: true,
            }),
        );

        this.singleJoinPipeline = [
            {
                $lookup: {
                    from: this.foreign1.getName(),
                    as: "f1",
                    localField: "a",
                    foreignField: "a",
                },
            },
            {$unwind: "$f1"},
        ];
        this.twoJoinPipeline = [
            ...this.singleJoinPipeline,
            {
                $lookup: {
                    from: this.foreign2.getName(),
                    as: "f2",
                    localField: "b",
                    foreignField: "b",
                },
            },
            {$unwind: "$f2"},
        ];

        // Profile every command with slowms: -1, so that each query below produces both a profiler
        // entry and a "Slow query" log line whose plan summaries can be compared.
        assert.commandWorked(db.setProfilingLevel(2, {slowms: -1}));

        this.planSummaryCommentCounter = 0;

        this.assertSlowLogShape = (shape) => {
            const log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
            assert(
                findMatchingLogLine(log, {
                    msg: "Slow query",
                    ...shape,
                }),
                "the slow query log did not report the expected plan summary",
                {shape},
            );
        };

        this.assertProfilerAndSlowLogForPipeline = (pipeline, shape) => {
            const comment = `plan_summary_${this.planSummaryCommentCounter++}`;
            assert.gt(this.coll.aggregate(pipeline, {comment}).toArray().length, 0);

            // Validates that both slow query log & profiler use the same summary as expected.
            const entry = getLatestProfilerEntry(db, {
                op: "command",
                ns: this.coll.getFullName(),
                "command.comment": comment,
            });
            assert.eq(
                entry.planSummary,
                shape.planSummary,
                `Expected plan summary to equal ${shape.planSummary}`,
                entry,
            );
            assert.eq(
                entry.usedJoinOptimization,
                shape.usedJoinOptimization,
                `Expected usedJoinOptimization to equal ${shape.usedJoinOptimization}`,
                entry,
            );
            assert.eq(
                entry.fromPlanCache,
                shape.fromPlanCache,
                `Expected fromPlanCache to equal ${shape.fromPlanCache}`,
                entry,
            );
            assert.eq(
                entry.fallbackReason,
                shape.fallbackReason,
                `Expected fallbackReason to equal ${shape.fallbackReason}`,
                entry,
            );

            this.assertSlowLogShape(shape);
        };
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("logs relevant fields for a single join", function () {
        const local = this.coll.getFullName();
        const foreign1 = this.foreign1.getFullName();
        const planSummary = `HJ( f1 = ( COLLSCAN [${foreign1}] ), _ = ( COLLSCAN [${local}] ) )`;
        // Note: fromPlanCache is omitted when its false.
        this.assertProfilerAndSlowLogForPipeline(this.singleJoinPipeline, {
            planSummary,
            usedJoinOptimization: true,
        });
        this.assertProfilerAndSlowLogForPipeline(this.singleJoinPipeline, {
            planSummary,
            usedJoinOptimization: true,
            fromPlanCache: true,
        });

        // Now repeat for where we end the prefix early & log the reason. Should reuse plan cache.
        const pipeline = [
            ...this.singleJoinPipeline,
            {
                $lookup: {
                    from: this.foreign2.getName(),
                    as: "f2",
                    localField: "b",
                    foreignField: "b",
                },
            },
            {$unwind: {path: "$f2", preserveNullAndEmptyArrays: true}},
        ];
        this.assertProfilerAndSlowLogForPipeline(pipeline, {
            planSummary: `HJ( f1 = ( COLLSCAN [${foreign1}] ), _ = ( COLLSCAN [${local}] ) ), IXSCAN { b: 1, d: 1 }`,
            usedJoinOptimization: true,
            fromPlanCache: true,
            fallbackReason: "outerJoinUnwind",
        });
    });

    it("logs relevant fields for two joins", function () {
        const local = this.coll.getFullName();
        const foreign1 = this.foreign1.getFullName();
        const foreign2 = this.foreign2.getFullName();
        const planSummary = `HJ( f2 = ( COLLSCAN [${foreign2}] ), _ = ( HJ( f1 = ( COLLSCAN [${foreign1}] ), _ = ( COLLSCAN [${local}] ) ) ) )`;
        this.assertProfilerAndSlowLogForPipeline(this.twoJoinPipeline, {
            planSummary,
            usedJoinOptimization: true,
        });
        this.assertProfilerAndSlowLogForPipeline(this.twoJoinPipeline, {
            planSummary,
            usedJoinOptimization: true,
            fromPlanCache: true,
        });
    });

    it("logs as normal for a non-join query", function () {
        // That means no 'usedJoinOptimization' and 'fromPlanCache' is only set after the entry is activated.
        const pipeline = [{$match: {a: 1}}];
        const planSummary = "IXSCAN { a: 1 }";
        this.assertProfilerAndSlowLogForPipeline(pipeline, {planSummary});
        this.assertProfilerAndSlowLogForPipeline(pipeline, {planSummary});
        this.assertProfilerAndSlowLogForPipeline(pipeline, {
            planSummary,
            fromPlanCache: true,
        });
    });

    it("logs the fallback reason when a query falls back from join optimization", function () {
        const pipeline = [
            {
                $lookup: {
                    from: this.foreign1.getName(),
                    pipeline: [{$match: {a: {$gte: 0}}}],
                    as: "f1",
                },
            },
            {$unwind: "$f1"},
        ];
        this.assertProfilerAndSlowLogForPipeline(pipeline, {
            planSummary: "COLLSCAN",
            fallbackReason: "graphDisconnected",
        });
    });
});
