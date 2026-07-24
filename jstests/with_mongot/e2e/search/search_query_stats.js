/**
 * Verify the query shape that is outputted by $queryStats for $search and $searchMeta queries
 * with real mongot.
 *
 * Uses an empty collection so query stats are collected without needing actual search results.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertAggregatedMetricsSingleExec,
    getLatestQueryStatsEntry,
    getQueryStats,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";

TestData.pinToSingleMongos = true;

const collName = jsTestName();
const coll = db.getCollection(collName);

const searchQuery = {
    index: jsTestName() + "_index",
    text: {query: "cakes", path: "title"},
};

describe("$search query stats", function () {
    before(function () {
        coll.drop();

        // Create the collection so it has a UUID (required by real mongot). Insert and delete a
        // document to establish the collection.
        assert.commandWorked(coll.insertOne({_id: "temp"}));
        assert.commandWorked(coll.deleteOne({_id: "temp"}));

        // Reset state to prevent failures in burn_in tests which repeatedly run this test and
        // expect the execCount to be 1.
        resetQueryStatsStore(db.getMongo(), "1MB");
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryStatsSampleRate: 1}));
    });

    after(function () {
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryStatsSampleRate: 0}));
        coll.drop();
    });

    it("should anonymize the $searchMeta stage as one object", function () {
        assert.commandWorked(
            db.runCommand({
                aggregate: collName,
                pipeline: [{$searchMeta: searchQuery}],
                cursor: {},
            }),
        );

        const stats = getLatestQueryStatsEntry(db.getMongo(), {collName});
        assert.eq(stats.key.queryShape.pipeline, [{$searchMeta: "?object"}], stats);
        assertAggregatedMetricsSingleExec(stats, {docsExamined: 0, keysExamined: 0});
    });

    it("should group repeated $searchMeta executions into a single shape", function () {
        resetQueryStatsStore(db.getMongo(), "1MB");

        const numExecs = 3;
        for (let i = 0; i < numExecs; i++) {
            // Test with different literals to confirm they don't affect the anonymized shape.
            const query = Object.assign({}, searchQuery, {
                text: {query: "cakes" + i, path: "title"},
            });
            assert.commandWorked(
                db.runCommand({aggregate: collName, pipeline: [{$searchMeta: query}], cursor: {}}),
            );
        }

        const stats = getQueryStats(db, {collName});
        assert.eq(stats.length, 1, stats);
        assert.eq(stats[0].key.queryShape.pipeline, [{$searchMeta: "?object"}], stats[0]);
        assert.eq(stats[0].metrics.execCount, numExecs, stats[0]);
    });
});
