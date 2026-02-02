/**
 * Test $queryStats behavior for $vectorSearch with real mongot.
 *
 * Uses an empty collection so query stats are collected without needing actual search results.
 *
 * @tags: [
 *   requires_fcv_71,
 * ]
 */

import {before, describe, it} from "jstests/libs/mochalite.js";
import {getQueryStats, resetQueryStatsStore} from "jstests/libs/query/query_stats_utils.js";

TestData.pinToSingleMongos = true;

const collName = jsTestName();
const coll = db.getCollection(collName);

describe("$vectorSearch query stats", function () {
    let stats;

    before(function () {
        coll.drop();

        // Create the collection so it has a UUID (required by real mongot).
        // Insert and delete a document to establish the collection.
        assert.commandWorked(coll.insert({_id: "temp"}));
        assert.commandWorked(coll.deleteOne({_id: "temp"}));

        // Needed to reset state and prevent failures in burn_in tests which repeatedly run this test and expect the execCount to be 1.
        resetQueryStatsStore(db.getMongo(), "1MB");
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryStatsRateLimit: -1}));

        // Query shape is determined by 'filter' and 'index' fields only.
        // 'queryVector', 'limit', and 'numCandidates' values don't affect shape but are tracked
        // in metrics.

        // First shape: filter + index="index_1" (2 executions with different vectors/limits)
        assert.commandWorked(
            db.runCommand({
                aggregate: collName,
                pipeline: [
                    {
                        $vectorSearch: {
                            filter: {a: {$gt: 5}},
                            index: "index_1",
                            path: "x",
                            queryVector: [1.0, 2.0, 3.0],
                            limit: 5,
                            numCandidates: 10,
                        },
                    },
                ],
                cursor: {},
            }),
        );

        assert.commandWorked(
            db.runCommand({
                aggregate: collName,
                pipeline: [
                    {
                        $vectorSearch: {
                            filter: {a: {$gt: 5}},
                            index: "index_1",
                            path: "x",
                            queryVector: [2.0, 3.0, 4.0],
                            limit: 10,
                            numCandidates: 10,
                        },
                    },
                ],
                cursor: {},
            }),
        );

        // Second shape: filter + index="index_2" (1 execution)
        assert.commandWorked(
            db.runCommand({
                aggregate: collName,
                pipeline: [
                    {
                        $vectorSearch: {
                            filter: {a: {$gt: 5}},
                            path: "x",
                            queryVector: [1.0, 0.0],
                            limit: 4,
                            numCandidates: 6,
                            index: "index_2",
                        },
                    },
                ],
                cursor: {},
            }),
        );

        // Third shape: only index="index_3" (1 execution)
        assert.commandWorked(
            db.runCommand({
                aggregate: collName,
                pipeline: [{$vectorSearch: {index: "index_3", path: "x", queryVector: [], numCandidates: 1, limit: 1}}],
                cursor: {},
            }),
        );

        stats = getQueryStats(db, {collName});
    });

    it("should collect 3 distinct query shapes", function () {
        assert.eq(stats.length, 3, stats);
    });

    it("should track shape with only index field", function () {
        const entry = stats[0];
        assert.docEq({$vectorSearch: {index: "index_3"}}, entry.key.queryShape.pipeline[0], entry);
        assert.eq(entry.metrics.execCount, 1, entry);

        // TODO (SERVER-118628): Uncomment this once extension vectorSearch stage supplemental metrics are supported.
        // const vectorSearchMetrics = getValueAtPath(entry.metrics, "supplementalMetrics.vectorSearch");
        // assert.docEq({sum: 1, min: 1, max: 1, sumOfSquares: 1}, vectorSearchMetrics.limit, entry);
        // assert.docEq({sum: 1, min: 1, max: 1, sumOfSquares: 1}, vectorSearchMetrics.numCandidatesLimitRatio, entry);
    });

    it("should group queries with same filter and index", function () {
        // Both queries 1 and 2 have same filter and index="index_1", so they share a shape.
        const entry = stats[1];
        assert.docEq(
            {$vectorSearch: {filter: {a: {$gt: "?number"}}, index: "index_1"}},
            entry.key.queryShape.pipeline[0],
            entry,
        );
        assert.eq(entry.metrics.execCount, 2, entry);

        // TODO (SERVER-118628): Uncomment this once extension vectorSearch stage supplemental metrics are supported.
        // const vectorSearchMetrics = getValueAtPath(entry.metrics, "supplementalMetrics.vectorSearch");
        // assert.docEq({sum: 15, min: 5, max: 10, sumOfSquares: 125}, vectorSearchMetrics.limit, entry);
        // assert.docEq({sum: 3, min: 1, max: 2, sumOfSquares: 5}, vectorSearchMetrics.numCandidatesLimitRatio, entry);
    });

    it("should track shape with filter and index", function () {
        const entry = stats[2];
        assert.docEq(
            {$vectorSearch: {filter: {a: {$gt: "?number"}}, index: "index_2"}},
            entry.key.queryShape.pipeline[0],
            entry,
        );
        assert.eq(entry.metrics.execCount, 1, entry);

        // TODO (SERVER-118628): Uncomment this once extension vectorSearch stage supplemental metrics are supported.
        //     const vectorSearchMetrics = getValueAtPath(entry.metrics, "supplementalMetrics.vectorSearch");
        //     assert.docEq({sum: 4, min: 4, max: 4, sumOfSquares: 16}, vectorSearchMetrics.limit, entry);
        //     assert.docEq(
        //         {sum: 1.5, min: 1.5, max: 1.5, sumOfSquares: 2.25},
        //         vectorSearchMetrics.numCandidatesLimitRatio,
        //         entry,
        //     );
    });
});
