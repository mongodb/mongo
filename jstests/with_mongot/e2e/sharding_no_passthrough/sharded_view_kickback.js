/**
 * Tests that a top-level $vectorSearch on a view, on a sharded cluster, correctly applies the
 * view's transform to idLookup'd documents even after the router has to retry the request through
 * a view-kickback (the shard didn't have the view's namespace resolved on the first attempt, so
 * it rejects the initial dispatch and the router retries against the resolved backing
 * collection). If the view's pipeline isn't correctly threaded through to the shards on retry,
 * results are missing the view's $addFields-added field.
 *
 * @tags: [
 *   featureFlagMongotIndexedViews,
 *   requires_sharding,
 *   assumes_unsharded_collection,
 *   requires_fcv_90,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getShardNames} from "jstests/libs/cluster_helpers/sharded_cluster_fixture_helpers.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieVectorSearchIndexSpec,
} from "jstests/with_mongot/e2e_lib/data/movies.js";

const dbName = jsTestName();
const testDb = db.getSiblingDB(dbName);

const collName = "moviesColl";
const coll = testDb[collName];

const enrichedViewName = "enrichedView";
const enrichedIndexName = "enriched_index";

describe("top-level $vectorSearch on a sharded view", function () {
    function dropLeftoverSearchIndex() {
        if (testDb.getCollectionInfos({name: enrichedViewName}).length === 0) {
            return;
        }
        const existing = testDb[enrichedViewName]
            .aggregate([{$listSearchIndexes: {name: enrichedIndexName}}])
            .toArray();
        if (existing.length > 0) {
            dropSearchIndex(testDb[enrichedViewName], {name: enrichedIndexName});
        }
    }

    before(function () {
        dropLeftoverSearchIndex();

        coll.drop();
        testDb[enrichedViewName].drop();

        if (FixtureHelpers.isMongos(testDb)) {
            const shardNames = getShardNames(testDb.getMongo());
            // Pin the primary shard so the chunk placement below is deterministic.
            assert.commandWorked(
                testDb.adminCommand({enableSharding: dbName, primaryShard: shardNames[0]}),
            );
            assert.commandWorked(coll.createIndex({_id: 1}));
            assert.commandWorked(coll.insertMany(getMovieData()));
            assert.commandWorked(
                testDb.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}),
            );
            // Split so the movie docs actually land on more than one shard: without this, the
            // whole pipeline may run unsplit on a single shard and never exercise the
            // DRM/idLookup split path this test targets.
            assert.commandWorked(
                testDb.adminCommand({split: coll.getFullName(), middle: {_id: 8}}),
            );
            if (shardNames.length > 1) {
                assert.commandWorked(
                    testDb.adminCommand({
                        moveChunk: coll.getFullName(),
                        find: {_id: 8},
                        to: shardNames[1],
                    }),
                );
            }
        } else {
            assert.commandWorked(coll.insertMany(getMovieData()));
        }

        // The view under test: adds a field the base collection doesn't have. If idLookup's
        // viewPipeline is dropped on shards, results will be missing "enriched_title".
        assert.commandWorked(
            testDb.createView(enrichedViewName, collName, [
                {
                    $addFields: {
                        enriched_title: {$concat: ["ENRICHED-", "$title"]},
                    },
                },
            ]),
        );

        createSearchIndex(
            testDb[enrichedViewName],
            getMovieVectorSearchIndexSpec({indexName: enrichedIndexName}),
        );
    });

    after(function () {
        dropLeftoverSearchIndex();
        testDb[enrichedViewName].drop();
        coll.drop();
    });

    it("applies the view's $addFields transform to every result", function () {
        const results = testDb[enrichedViewName]
            .aggregate([
                {
                    $vectorSearch: {
                        queryVector: getMoviePlotEmbeddingById(6),
                        path: "plot_embedding",
                        exact: true,
                        index: enrichedIndexName,
                        limit: 5,
                    },
                },
            ])
            .toArray();

        assert.gt(results.length, 0, "expected at least one result", {results});

        for (const doc of results) {
            assert(
                doc.hasOwnProperty("enriched_title"),
                "top-level $vectorSearch-on-view result is missing the view's " +
                    "$addFields-added 'enriched_title' field -- the view transform was not " +
                    "applied to this idLookup'd document",
                {doc},
            );
        }
    });
});
