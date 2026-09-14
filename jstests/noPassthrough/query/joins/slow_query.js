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
            db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}),
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

        this.assertPlanSummaryInSlowLog = (comment, expectedPlanSummary) => {
            const log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
            assert(
                findMatchingLogLine(log, {
                    msg: "Slow query",
                    comment,
                    planSummary: expectedPlanSummary,
                }),
                "the slow query log did not report the expected plan summary",
                {comment, planSummary: expectedPlanSummary},
            );
        };

        this.getPlanSummaryForPipeline = (pipeline) => {
            const comment = `plan_summary_${this.planSummaryCommentCounter++}`;
            assert.gt(this.coll.aggregate(pipeline, {comment}).toArray().length, 0);

            // Validates that both slow query log & profiler use the same summary, then return it.
            const summary = getLatestProfilerEntry(db, {
                op: "command",
                ns: this.coll.getFullName(),
                "command.comment": comment,
            }).planSummary;
            this.assertPlanSummaryInSlowLog(comment, summary);
            return summary;
        };
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("logs a plan summary for a single join", function () {
        const local = this.coll.getFullName();
        const foreign1 = this.foreign1.getFullName();
        assert.eq(
            `HJ( f1 = ( COLLSCAN [${foreign1}] ), _ = ( COLLSCAN [${local}] ) )`,
            this.getPlanSummaryForPipeline(this.singleJoinPipeline),
        );
    });

    it("logs a plan summary for two joins", function () {
        const local = this.coll.getFullName();
        const foreign1 = this.foreign1.getFullName();
        const foreign2 = this.foreign2.getFullName();
        assert.eq(
            `HJ( f2 = ( COLLSCAN [${foreign2}] ), _ = ( HJ( f1 = ( COLLSCAN [${foreign1}] ), _ = ( COLLSCAN [${local}] ) ) ) )`,
            this.getPlanSummaryForPipeline(this.twoJoinPipeline),
        );
    });
});
