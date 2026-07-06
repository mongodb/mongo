/**
 * Tests that server status tracks aggregation pipelines that run without holding an execution
 * ticket (SERVER-127796). The metrics are recorded under
 * query.aggregation.nonTicketed.{intervals,totalMillis,queries}.
 *
 * A non-ticketed interval occurs between a $cursor stage (which holds a ticket while reading) and
 * a subsequent in-memory stage such as $sort or $group. Classic's $sort/$group DocumentSources sit
 * outside the yield-instrumented PlanStage tree, so without this deliberate release they'd hold one
 * ticket for the whole blocking operation; that's specific to the classic engine. When SBE pushes
 * $sort/$group into the query solution, they run as ordinary PlanStages and yield via the normal
 * PlanYieldPolicy, so there's no comparable gap for this metric to observe. Force the classic
 * engine here so the assertions below hold regardless of which engine a given variant defaults to;
 * a couple of tests below override this to trySbeEngine to verify the SBE-specific behavior
 * directly.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        delinquentAcquisitionIntervalMillis: 10,
        internalQueryFrameworkControl: "forceClassicEngine",
    },
});
assert.neq(null, conn, "mongod failed to start");

const db = conn.getDB("test");
const coll = db.agg_non_ticketed_metrics;

describe("query.aggregation.nonTicketed serverStatus metrics", function () {
    before(function () {
        coll.drop();
        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < 100; i++) {
            bulk.insert({x: i, y: i});
        }
        assert.commandWorked(bulk.execute());
        // Create an index on y so that $cursor uses it, but x has no index, forcing in-memory sort.
        assert.commandWorked(coll.createIndex({y: 1}));
    });

    it("counters exist in serverStatus with initial value 0", function () {
        const stats = assert.commandWorked(db.adminCommand({serverStatus: 1}));
        const nonTicketed = stats.metrics.query.aggregation.nonTicketed;
        assert(nonTicketed !== undefined, "nonTicketed metrics should exist", {stats});
        assert.eq(nonTicketed.intervals, 0, "intervals should start at 0");
        assert.eq(nonTicketed.totalMillis, 0, "totalMillis should start at 0");
        assert.eq(nonTicketed.maxMillis, 0, "maxMillis should start at 0");
        assert.eq(nonTicketed.queries, 0, "queries should start at 0");
    });

    it("running an aggregate with blocking $sort increments the queries counter", function () {
        // Use the failpoint to guarantee the non-ticketed interval exceeds the 10ms threshold,
        // making this test deterministic regardless of machine speed. DocumentSourceCursor
        // can read in multiple batches; using "times: 1" here ensures only the first of those
        // release/re-acquire cycles is delayed past the threshold, so exactly one non-ticketed
        // interval is counted.
        assert.commandWorked(
            db.adminCommand({
                configureFailPoint: "sleepAfterReleasingAggTicket",
                mode: {times: 1},
                data: {waitTimeMillis: 50},
            }),
        );

        const statsBefore = assert.commandWorked(db.adminCommand({serverStatus: 1})).metrics.query
            .aggregation.nonTicketed;

        // Use a scan on y (indexed) but sort on x (no index) to force an in-memory sort.
        // The $sort runs without a ticket after all docs are read via $cursor.
        assert.commandWorked(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [{$match: {y: {$lt: 100}}}, {$sort: {x: -1}}, {$count: "total"}],
                cursor: {},
            }),
        );

        assert.commandWorked(
            db.adminCommand({configureFailPoint: "sleepAfterReleasingAggTicket", mode: "off"}),
        );

        const statsAfter = assert.commandWorked(db.adminCommand({serverStatus: 1})).metrics.query
            .aggregation.nonTicketed;

        assert.eq(statsAfter.queries, statsBefore.queries + 1, "queries should increase by 1");
        assert.eq(
            statsAfter.intervals,
            statsBefore.intervals + 1,
            "intervals should increase by 1",
        );
        assert.gt(statsAfter.totalMillis, statsBefore.totalMillis, "totalMillis should increase");
    });

    it("fully pushed-down SBE aggregation does not increment the non-ticketed counters", function () {
        // A pipeline that SBE can fully absorb (scan + sort + group) collapses into a single
        // PlanExecutor call with no boundary between a $cursor and in-memory stage. SBE's sort and
        // group are ordinary PlanStages that yield via PlanYieldPolicy, so there's no long-held gap
        // here for this metric to observe.
        const prevFramework = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
        ).internalQueryFrameworkControl;
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}),
        );
        try {
            const pipeline = [{$match: {y: {$lt: 100}}}, {$sort: {x: -1}}, {$count: "total"}];
            const explainRes = assert.commandWorked(
                db.runCommand({
                    explain: {aggregate: coll.getName(), pipeline, cursor: {}},
                    verbosity: "queryPlanner",
                }),
            );
            assert(
                explainRes.queryPlanner.optimizedPipeline,
                "expected $sort/$count to be pushed down into a single SBE plan",
                {explainRes},
            );

            assert.commandWorked(
                db.adminCommand({
                    configureFailPoint: "sleepAfterReleasingAggTicket",
                    mode: {times: 1},
                    data: {waitTimeMillis: 50},
                }),
            );
            try {
                const statsBefore = assert.commandWorked(db.adminCommand({serverStatus: 1})).metrics
                    .query.aggregation.nonTicketed;

                assert.commandWorked(
                    db.runCommand({aggregate: coll.getName(), pipeline, cursor: {}}),
                );

                const statsAfter = assert.commandWorked(db.adminCommand({serverStatus: 1})).metrics
                    .query.aggregation.nonTicketed;

                assert.eq(
                    statsAfter,
                    statsBefore,
                    "no non-ticketed interval should be recorded when the whole pipeline is " +
                        "pushed into a single SBE plan",
                    {statsBefore, statsAfter},
                );
            } finally {
                assert.commandWorked(
                    db.adminCommand({
                        configureFailPoint: "sleepAfterReleasingAggTicket",
                        mode: "off",
                    }),
                );
            }
        } finally {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryFrameworkControl: prevFramework}),
            );
        }
    });

    it("SBE-backed scan with a non-pushed-down blocking stage still increments the counters", function () {
        // $accumulator runs custom JS, which SBE cannot execute, so this $group stays a classic
        // DocumentSource stage even though the scan itself is SBE-backed. It should be tracked.
        const prevFramework = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
        ).internalQueryFrameworkControl;
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}),
        );
        try {
            const pipeline = [
                {$match: {y: {$lt: 100}}},
                {
                    $group: {
                        _id: null,
                        total: {
                            $accumulator: {
                                init: function () {
                                    return 0;
                                },
                                accumulate: function (state, x) {
                                    return state + x;
                                },
                                accumulateArgs: ["$x"],
                                merge: function (s1, s2) {
                                    return s1 + s2;
                                },
                                lang: "js",
                            },
                        },
                    },
                },
            ];

            const explainRes = assert.commandWorked(
                db.runCommand({
                    explain: {aggregate: coll.getName(), pipeline, cursor: {}},
                    verbosity: "queryPlanner",
                }),
            );
            assert(
                explainRes.stages[0].$cursor.queryPlanner.winningPlan.slotBasedPlan,
                "expected the scan to run via SBE",
                {explainRes},
            );
            assert(
                explainRes.stages[1].$group,
                "expected $group to remain a separate classic DocumentSource stage",
                {explainRes},
            );

            assert.commandWorked(
                db.adminCommand({
                    configureFailPoint: "sleepAfterReleasingAggTicket",
                    mode: {times: 1},
                    data: {waitTimeMillis: 50},
                }),
            );
            try {
                const statsBefore = assert.commandWorked(db.adminCommand({serverStatus: 1})).metrics
                    .query.aggregation.nonTicketed;

                assert.commandWorked(
                    db.runCommand({aggregate: coll.getName(), pipeline, cursor: {}}),
                );

                const statsAfter = assert.commandWorked(db.adminCommand({serverStatus: 1})).metrics
                    .query.aggregation.nonTicketed;

                assert.eq(
                    statsAfter.queries,
                    statsBefore.queries + 1,
                    "queries should increase by 1",
                );
                assert.eq(
                    statsAfter.intervals,
                    statsBefore.intervals + 1,
                    "intervals should increase by 1",
                );
            } finally {
                assert.commandWorked(
                    db.adminCommand({
                        configureFailPoint: "sleepAfterReleasingAggTicket",
                        mode: "off",
                    }),
                );
            }
        } finally {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryFrameworkControl: prevFramework}),
            );
        }
    });

    it("idle time between getMore calls is not counted as non-ticketed", function () {
        // Open a multi-batch cursor. The blocking $sort runs entirely within the aggregate
        // command, so any non-ticketed interval is already closed by the time the first batch
        // is returned.
        const aggRes = assert.commandWorked(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [{$match: {y: {$lt: 100}}}, {$sort: {x: -1}}],
                cursor: {batchSize: 1},
            }),
        );
        const cursorId = aggRes.cursor.id;
        assert.neq(cursorId, 0, "expected an open cursor");

        // Capture metrics after the aggregate command — the sort interval is already flushed.
        const statsAfterAgg = assert.commandWorked(db.adminCommand({serverStatus: 1})).metrics.query
            .aggregation.nonTicketed;

        // Sleep much longer than the 10ms threshold to simulate client idle time between
        // getMore calls. Each getMore runs on a new opCtx, so this idle time must not be
        // attributed to any non-ticketed interval.
        sleep(500);

        // Exhaust the cursor.
        let cursor = cursorId;
        while (cursor != 0) {
            const getMoreRes = assert.commandWorked(
                db.runCommand({getMore: cursor, collection: coll.getName()}),
            );
            cursor = getMoreRes.cursor.id;
        }

        const statsAfterGetMore = assert.commandWorked(db.adminCommand({serverStatus: 1})).metrics
            .query.aggregation.nonTicketed;

        // The 500ms inter-getMore idle time must not appear in the counters.
        assert.eq(
            statsAfterGetMore.intervals,
            statsAfterAgg.intervals,
            "inter-getMore idle time must not be counted as a non-ticketed interval",
        );
        assert.eq(
            statsAfterGetMore.totalMillis,
            statsAfterAgg.totalMillis,
            "inter-getMore idle time must not be counted in totalMillis",
        );
        assert.eq(
            statsAfterGetMore.queries,
            statsAfterAgg.queries,
            "no additional queries should be recorded for getMore commands",
        );
    });
});

after(function () {
    MongoRunner.stopMongod(conn);
});
