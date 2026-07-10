/**
 * Verify the query shape that is outputted by $queryStats for $search and $searchMeta queries
 * with real mongot.
 *
 * Uses an empty collection so query stats are collected without needing actual search results.
 * E2E version of jstests/with_mongot/search_mocked/search_query_stats.js
 *
 * @tags: [
 *   # TODO (SERVER-131073): remove this tag once $searchMeta query-stats collection on mongos is fixed.
 *   assumes_against_mongod_not_mongos,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertAggregatedMetricsSingleExec,
    getLatestQueryStatsEntry,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";

TestData.pinToSingleMongos = true;

const collName = jsTestName();
const coll = db.getCollection(collName);

const searchQuery = {
    index: "search_query_stats_index",
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

    it("should anonymize the $search stage as one object", function () {
        assert.commandWorked(
            db.runCommand({aggregate: collName, pipeline: [{$search: searchQuery}], cursor: {}}),
        );

        const stats = getLatestQueryStatsEntry(db.getMongo(), {collName});
        assert.eq(stats.key.queryShape.pipeline, [{$search: "?object"}], stats);
        // The collection is empty, so the $_internalSearchIdLookup stage examines no documents.
        assertAggregatedMetricsSingleExec(stats, {docsExamined: 0, keysExamined: 0});
    });
});
