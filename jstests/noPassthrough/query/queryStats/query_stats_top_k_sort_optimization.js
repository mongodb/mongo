/**
 * Tests the $queryStats top-K sort+limit optimization:
 * - Detects $queryStats -> [optional $project] -> $sort(absorbed $limit) on a supported metric,
 *   annotated on the $queryStats stage as 'topKSortOptimization' in explain
 * - Produces correct results on both the optimized and unoptimized paths
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";
import {resetQueryStatsStore} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

/**
 * Returns the 'topKSortOptimization' hint stored on the $queryStats stage in the explain, or null
 * if the optimization did not fire.
 */
function getOptimizationSpec(adminDB, pipeline) {
    const explain = assert.commandWorked(
        adminDB.runCommand({
            aggregate: 1,
            pipeline: pipeline,
            explain: true,
            cursor: {},
        }),
    );

    const root = explain.mongos ?? explain;
    const queryStatsStage = getAggPlanStage(root, "$queryStats");
    assert(queryStatsStage, "expected a $queryStats stage in EXPLAIN", {explain});
    return queryStatsStage["$queryStats"].topKSortOptimization ?? null;
}

function withTopKOptimization(adminDB, enabled, fn) {
    const knobName = "internalQueryStatsTopKSortOptimizationEnabled";
    const prev = assert.commandWorked(adminDB.adminCommand({getParameter: 1, [knobName]: 1}))[
        knobName
    ];
    assert.commandWorked(adminDB.runCommand({setParameter: 1, [knobName]: enabled}));
    try {
        return fn();
    } finally {
        assert.commandWorked(adminDB.runCommand({setParameter: 1, [knobName]: prev}));
    }
}

/**
 * Registers all test cases for a given cluster topology.
 */
function defineTopKTests(startCluster, stopCluster) {
    let conn, coll, testDB, adminDB;
    const numShapes = 20;

    before(function () {
        ({conn, adminDB, testDB} = startCluster());
        coll = testDB[jsTestName()];
    });

    beforeEach(function () {
        coll.drop();
        // Insert some documents, so the find queries have some work to do.
        let docs = [];
        for (let i = 0; i < numShapes; i++) {
            docs.push({["field" + i]: i});
        }
        assert.commandWorked(coll.insertMany(docs));
        resetQueryStatsStore(conn, "1MB");

        // Run some find queries, so the store isn't empty.
        for (let i = 0; i < numShapes; i++) {
            coll.find({["field" + i]: i}).itcount();
        }
    });

    after(function () {
        stopCluster();
    });

    describe("optimization appears on explain", function () {
        it("appears on a minimal pipeline without a $project", function () {
            const pipeline = [{$queryStats: {}}, {$sort: {"metrics.execCount": -1}}, {$limit: 5}];
            const spec = getOptimizationSpec(adminDB, pipeline);
            assert(spec !== null, "expected the top-K hint to be set", {spec});
            assert.eq(spec.path, "metrics.execCount", {spec});
            assert.eq(spec.isAscending, false, {spec});
            assert.eq(spec.limit, 5, {spec});
        });

        it("does not appear when the optimization doesn't apply", function () {
            const pipeline = [
                {$queryStats: {}},
                {$sort: {"metrics.queryExec.readTimeMicros": -1}},
                {$limit: 5},
            ];
            const spec = getOptimizationSpec(adminDB, pipeline);
            assert(spec === null, "expected no hint for a non-whitelisted metric", {spec});
        });
    });

    describe("correctness: optimization produces the correct result", function () {
        beforeEach(function () {
            // Reset the store and populate it with varied execCounts so that $sort on execCount
            // produces a meaningful ordering.
            resetQueryStatsStore(conn, "1MB");
            const extraRuns = {3: 2, 4: 5, 8: 10, 14: 9, 19: 1};
            for (let i = 0; i < numShapes; i++) {
                const total = 1 + (extraRuns[i] ?? 0);
                for (let j = 0; j < total; j++) {
                    coll.find({["field" + i]: i}).itcount();
                }
            }
        });

        it("ascending sort", function () {
            // Return almost all of the entries in the query stats store to make sure some shapes
            // have a different 'execCount'.
            const pipeline = [
                {$queryStats: {}},
                {$sort: {"metrics.execCount": 1}},
                {$limit: numShapes - 3},
            ];

            const spec = getOptimizationSpec(adminDB, pipeline);
            assert(spec !== null, "expected the top-K hint to be set", {spec});
            assert.eq(spec.path, "metrics.execCount", {spec});
            assert.eq(spec.isAscending, true, {spec});
            assert.eq(spec.limit, numShapes - 3, {spec});

            const results = adminDB.aggregate(pipeline).toArray();
            for (let i = 1; i < results.length; i++) {
                assert.lte(
                    results[i - 1].metrics.execCount,
                    results[i].metrics.execCount,
                    "results not sorted ascending by execCount",
                    {prev: results[i - 1], curr: results[i]},
                );
            }
        });

        it("returns the same documents with and without the optimization", function () {
            const pipeline = [{$queryStats: {}}, {$sort: {"metrics.execCount": -1}}, {$limit: 3}];

            const optimized = adminDB.aggregate(pipeline).toArray();
            const unoptimized = withTopKOptimization(adminDB, false, function () {
                return adminDB.aggregate(pipeline).toArray();
            });

            assert.eq(
                optimized.length,
                unoptimized.length,
                "result count differs between optimized and unoptimized",
                {optimized, unoptimized},
            );

            assert.sameMembers(
                optimized.map((d) => d.keyHash),
                unoptimized.map((d) => d.keyHash),
                "keyHash sets differ between optimized and unoptimized paths",
            );
        });

        it("correct results when a $match prevents the optimization", function () {
            // The intervening $match blocks the optimization, so this exercises the plain
            // $queryStats -> $match -> $sort -> $limit path.
            // TODO SERVER-125570 this test will break once the $match optimization is enabled.
            // Change this test once the optimization is enabled.
            const all = adminDB.aggregate([{$queryStats: {}}]).toArray();

            // Pick hashes with different execution counts so we can validate the $sort worked.
            // We use execution count to make the test deterministic.
            const hashesWithExecCount = (n) =>
                all.filter((d) => friendlyEqual(d.metrics.execCount, n)).map((d) => d.keyHash);
            const keyHashes = [
                ...hashesWithExecCount(11),
                ...hashesWithExecCount(10),
                ...hashesWithExecCount(2),
                ...hashesWithExecCount(1).slice(0, 3),
            ];
            assert.eq(keyHashes.length, 6, "unexpected store contents", {all});

            const limit = 4;
            const pipeline = [
                {$queryStats: {}},
                {$match: {keyHash: {$in: keyHashes}}},
                {$sort: {"metrics.execCount": -1}},
                {$limit: limit},
            ];

            const spec = getOptimizationSpec(adminDB, pipeline);
            assert(spec === null, "expected no hint with an intervening $match", {spec});

            const expectedExecCounts = [11, 10, 2, 1];

            const results = adminDB.aggregate(pipeline).toArray();
            assert.eq(results.length, limit, {results, keyHashes});
            for (let i = 0; i < results.length; i++) {
                assert(
                    friendlyEqual(results[i].metrics.execCount, expectedExecCounts[i]),
                    "unexpected execCount at position " + i,
                    {results},
                );
            }
        });

        it("returns the correct cardinality and top group with tied execCounts", function () {
            // Five shapes share the highest execCount and the limit is 3. Both the optimized
            // top-K heap and the unoptimized $sort+$limit may pick any 3 of the 5 tied entries,
            // so only assert what is guaranteed: exactly 'limit' rows, all from the top tie
            // group.
            resetQueryStatsStore(conn, "1MB");
            for (let i = 0; i < numShapes; i++) {
                const runs = i < 5 ? 3 : 1;
                for (let j = 0; j < runs; j++) {
                    coll.find({["field" + i]: i}).itcount();
                }
            }

            const pipeline = [{$queryStats: {}}, {$sort: {"metrics.execCount": -1}}, {$limit: 3}];

            const spec = getOptimizationSpec(adminDB, pipeline);
            assert(spec !== null, "expected the top-K hint to be set", {spec});

            const results = adminDB.aggregate(pipeline).toArray();
            assert.eq(results.length, 3, {results});
            for (const result of results) {
                assert(
                    friendlyEqual(result.metrics.execCount, 3),
                    "expected a result from the top tie group",
                    {result},
                );
            }
        });
    });
}

describe("$queryStats top-K sort (standalone)", function () {
    let conn;
    defineTopKTests(
        function startCluster() {
            conn = MongoRunner.runMongod({
                setParameter: {
                    internalQueryStatsSampleRate: 1,
                    internalQueryStatsCacheSize: "1MB",
                    // TODO SERVER-135111: remove once the optimization is enabled by default.
                    internalQueryStatsTopKSortOptimizationEnabled: true,
                },
            });
            return {conn, adminDB: conn.getDB("admin"), testDB: conn.getDB("test")};
        },
        function stopCluster() {
            MongoRunner.stopMongod(conn);
        },
    );
});

describe("$queryStats top-K sort (on mongos)", function () {
    let st;
    defineTopKTests(
        function startCluster() {
            st = new ShardingTest({
                shards: 2,
                other: {
                    mongosOptions: {
                        setParameter: {
                            internalQueryStatsSampleRate: 1,
                            internalQueryStatsCacheSize: "1MB",
                            // TODO SERVER-135111: remove once the optimization is enabled by
                            // default.
                            internalQueryStatsTopKSortOptimizationEnabled: true,
                        },
                    },
                },
            });
            return {conn: st.s, adminDB: st.s.getDB("admin"), testDB: st.s.getDB("test")};
        },
        function stopCluster() {
            st.stop();
        },
    );
});

describe("$queryStats top-K sort (on a shard server)", function () {
    let st;
    defineTopKTests(
        function startCluster() {
            st = new ShardingTest({
                shards: 1,
                other: {
                    rs: {
                        setParameter: {
                            internalQueryStatsSampleRate: 1,
                            internalQueryStatsCacheSize: "1MB",
                            // TODO SERVER-135111: remove once the optimization is enabled by
                            // default.
                            internalQueryStatsTopKSortOptimizationEnabled: true,
                        },
                    },
                },
            });
            const shardPrimary = st.rs0.getPrimary();
            return {
                conn: shardPrimary,
                adminDB: shardPrimary.getDB("admin"),
                testDB: shardPrimary.getDB("test"),
            };
        },
        function stopCluster() {
            st.stop();
        },
    );
});
