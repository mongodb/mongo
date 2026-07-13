/**
 * Tests $vectorSearch inside $lookup subpipelines for three structural shapes: identity views
 * (empty-pipeline views), nested $lookups (two levels deep), and dual-view $lookups (two
 * independent $lookups in one pipeline). Each case compares against top-level $vectorSearch
 * on the relevant view/collection as ground truth.
 *
 * @tags: [
 *   featureFlagMongotIndexedViews,
 *   requires_fcv_90,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";
import {
    createMoviesCollAndVectorIndex,
    createMoviesViewAndIndex,
    getMoviePlotEmbeddingById,
    getMovieVectorSearchIndexSpec,
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {
    assertDocArrExpectedFuzzy,
    assertIfVectorSearchNotAllowedInLookup,
    datasets,
    stripScores,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

// -------------------------------------------------------------------------
// Shared view/collection handles — reuse via checkForExistingIndex.
// -------------------------------------------------------------------------
const enrichedView = createMoviesViewAndIndex(datasets.MOVIES_WITH_ENRICHED_TITLE);
const actionView = createMoviesViewAndIndex(datasets.ACTION_MOVIES);
const moviesColl = createMoviesCollAndVectorIndex();

const viewDb = enrichedView.getDB();

const kLimit = 5;

// -------------------------------------------------------------------------
// Identity view: an empty-pipeline view over moviesColl, indexed separately.
// We create this in the same DB so $lookup can resolve it.
// -------------------------------------------------------------------------
const kIdentityViewName = jsTestName() + "_identity_view";
const kIdentityIndexName = jsTestName() + "_identity_idx";

function makeVectorSearchStage(indexName, limit) {
    return {
        $vectorSearch: {
            queryVector: getMoviePlotEmbeddingById(6),
            path: "plot_embedding",
            exact: true,
            index: indexName,
            limit: limit,
        },
    };
}

function makeSubPipeline(indexName, limit = kLimit) {
    return [
        makeVectorSearchStage(indexName, limit),
        {$project: {_id: 1, title: 1, score: {$meta: "vectorSearchScore"}}},
    ];
}

describe("$vectorSearch in $lookup: identity view, nested $lookup, and dual-view $lookup cases", function () {
    let identityView;

    // Local collections created by this file; named with jsTestName() prefix.
    const localCollName = jsTestName() + "_local";
    const localColl = viewDb.getCollection(localCollName);
    const outerCollName = jsTestName() + "_outer";
    const outerColl = viewDb.getCollection(outerCollName);

    before(function () {
        // Create the identity view (empty-pipeline view over moviesColl).
        const existingInfos = viewDb.getCollectionInfos({name: kIdentityViewName, type: "view"});
        if (existingInfos.length === 0) {
            assert.commandWorked(viewDb.createView(kIdentityViewName, moviesColl.getName(), []));
        }
        identityView = viewDb.getCollection(kIdentityViewName);

        // Create a vector index on the identity view if needed.
        const indexSpec = getMovieVectorSearchIndexSpec({indexName: kIdentityIndexName});
        const existingIndexes = identityView.aggregate([{$listSearchIndexes: {}}]).toArray();
        if (!existingIndexes.some((idx) => idx.name === kIdentityIndexName)) {
            createSearchIndex(identityView, indexSpec);
        }

        // Populate the local collection (outer side).
        localColl.drop();
        assert.commandWorked(
            localColl.insertMany([
                {_id: 400, label: "doc_a"},
                {_id: 401, label: "doc_b"},
            ]),
        );

        // Populate the outer collection for the nested-$lookup case.
        outerColl.drop();
        assert.commandWorked(
            outerColl.insertMany([
                {_id: 500, label: "outer_a"},
                {_id: 501, label: "outer_b"},
            ]),
        );

        // Prime routing info.
        localColl
            .aggregate([{$lookup: {from: kIdentityViewName, pipeline: [], as: "out"}}])
            .toArray();
        localColl
            .aggregate([{$lookup: {from: enrichedView.getName(), pipeline: [], as: "out"}}])
            .toArray();
        localColl
            .aggregate([{$lookup: {from: actionView.getName(), pipeline: [], as: "out"}}])
            .toArray();
        outerColl.aggregate([{$lookup: {from: localCollName, pipeline: [], as: "out"}}]).toArray();
    });

    after(function () {
        // Drop only private collections; leave shared views/indexes.
        localColl.drop();
        outerColl.drop();
        // Drop the identity view index since it's private to this test.
        try {
            dropSearchIndex(identityView, {name: kIdentityIndexName});
        } catch (e) {
            // Best effort — don't fail cleanup.
            jsTest.log.info("best-effort index drop failed", {error: e});
        }
        // Drop the identity view itself (private to this test file).
        assert.commandWorked(viewDb.runCommand({drop: kIdentityViewName}));
    });

    describe("$lookup from an identity view with $vectorSearch", function () {
        it("returns results consistent with top-level $vectorSearch on the underlying collection", function () {
            const subPipeline = makeSubPipeline(kIdentityIndexName);

            // Ground truth: top-level $vectorSearch on the identity view.
            const expectedFromView = identityView.aggregate(subPipeline).toArray();
            assert.gt(
                expectedFromView.length,
                0,
                "ground-truth query on identity view returned no results",
                {
                    expectedFromView,
                },
            );

            // The identity view has no transforms — results should equal underlying coll query
            // with the same index.
            assert.eq(
                expectedFromView.length,
                kLimit,
                "identity-view ground truth returned unexpected count",
                {
                    expectedFromView,
                },
            );

            const pipeline = [
                {
                    $lookup: {
                        from: kIdentityViewName,
                        pipeline: subPipeline,
                        as: "movies",
                    },
                },
                {$sort: {_id: 1}},
            ];
            assertIfVectorSearchNotAllowedInLookup(
                viewDb,
                () => localColl.runCommand("aggregate", {pipeline, cursor: {}}),
                () => {
                    const results = localColl.aggregate(pipeline).toArray();

                    assert.eq(results.length, 2, "expected one result per local document", {
                        results,
                    });

                    for (const resultDoc of results) {
                        assert.eq(
                            resultDoc.movies.length,
                            kLimit,
                            "expected kLimit movies in identity-view lookup result",
                            {
                                resultDoc,
                            },
                        );

                        // Scores sorted descending.
                        for (let i = 1; i < resultDoc.movies.length; i++) {
                            assert.lte(
                                resultDoc.movies[i].score,
                                resultDoc.movies[i - 1].score,
                                "scores not sorted descending in identity-view lookup",
                                {resultDoc},
                            );
                        }

                        // Results must match ground truth.
                        assertDocArrExpectedFuzzy(
                            stripScores(expectedFromView),
                            stripScores(resultDoc.movies),
                        );
                    }
                },
            );
        });
    });

    describe("Nested $lookup where the innermost lookup targets a view with $vectorSearch", function () {
        it("returns correct inner vector-search results nested two levels deep", function () {
            // Ground truth at the innermost level.
            const innerSubPipelineWithField = makeSubPipeline(
                datasets.MOVIES_WITH_ENRICHED_TITLE.indexName,
            ).map((stage) => {
                if (stage.$project) {
                    return {$project: Object.assign({}, stage.$project, {enriched_title: 1})};
                }
                return stage;
            });

            const expectedInner = enrichedView.aggregate(innerSubPipelineWithField).toArray();
            assert.gt(expectedInner.length, 0, "inner ground truth returned no results");

            // Nested pipeline:
            // outerColl -> $lookup from localColl
            //   localColl subpipeline: $lookup from enrichedView with $vectorSearch
            const pipeline = [
                {
                    $lookup: {
                        from: localCollName,
                        pipeline: [
                            {
                                $lookup: {
                                    from: enrichedView.getName(),
                                    pipeline: innerSubPipelineWithField,
                                    as: "inner_movies",
                                },
                            },
                        ],
                        as: "local_docs",
                    },
                },
                {$sort: {_id: 1}},
            ];

            assertIfVectorSearchNotAllowedInLookup(
                viewDb,
                () => outerColl.runCommand("aggregate", {pipeline, cursor: {}}),
                () => {
                    const results = outerColl.aggregate(pipeline).toArray();
                    assert.eq(results.length, 2, "expected one result per outer document", {
                        results,
                    });

                    for (const outerDoc of results) {
                        // Each outer doc joined all local docs.
                        assert.gt(outerDoc.local_docs.length, 0, "outer doc has no local_docs", {
                            outerDoc,
                        });

                        for (const localDoc of outerDoc.local_docs) {
                            // Every local doc must carry the inner movies.
                            assert.eq(
                                localDoc.inner_movies.length,
                                expectedInner.length,
                                "inner nested lookup returned unexpected count",
                                {localDoc},
                            );

                            // Each inner movie must have the view-added enriched_title.
                            for (const movie of localDoc.inner_movies) {
                                assert(
                                    movie.hasOwnProperty("enriched_title"),
                                    "inner nested movie missing enriched_title from view transform",
                                    {movie},
                                );
                            }

                            // Inner movies must match ground truth.
                            assertDocArrExpectedFuzzy(
                                stripScores(expectedInner),
                                stripScores(localDoc.inner_movies),
                            );
                        }
                    }
                },
            );
        });
    });

    describe("Two $lookups in one pipeline, each with $vectorSearch on a different view", function () {
        it("returns independent correct results from two view-targeted lookups", function () {
            const enrichedSubPipeline = makeSubPipeline(
                datasets.MOVIES_WITH_ENRICHED_TITLE.indexName,
            ).map((stage) => {
                if (stage.$project) {
                    return {$project: Object.assign({}, stage.$project, {enriched_title: 1})};
                }
                return stage;
            });
            const actionSubPipeline = makeSubPipeline(datasets.ACTION_MOVIES.indexName, kLimit);

            // Ground truths.
            const enrichedExpected = enrichedView.aggregate(enrichedSubPipeline).toArray();
            const actionExpected = actionView.aggregate(actionSubPipeline).toArray();

            assert.gt(enrichedExpected.length, 0, "enriched view ground truth empty");
            assert.gt(actionExpected.length, 0, "action view ground truth empty");

            // Run the dual-lookup pipeline.
            const pipeline = [
                {
                    $lookup: {
                        from: enrichedView.getName(),
                        pipeline: enrichedSubPipeline,
                        as: "enriched_movies",
                    },
                },
                {
                    $lookup: {
                        from: actionView.getName(),
                        pipeline: actionSubPipeline,
                        as: "action_movies",
                    },
                },
                {$sort: {_id: 1}},
            ];
            assertIfVectorSearchNotAllowedInLookup(
                viewDb,
                () => localColl.runCommand("aggregate", {pipeline, cursor: {}}),
                () => {
                    const results = localColl.aggregate(pipeline).toArray();

                    assert.eq(results.length, 2, "expected one result per local document", {
                        results,
                    });

                    for (const resultDoc of results) {
                        // Enriched view results must match ground truth.
                        assert.eq(
                            resultDoc.enriched_movies.length,
                            enrichedExpected.length,
                            "enriched_movies count mismatch",
                            {
                                resultDoc,
                            },
                        );
                        for (const movie of resultDoc.enriched_movies) {
                            assert(
                                movie.hasOwnProperty("enriched_title"),
                                "enriched_movies doc missing enriched_title",
                                {
                                    movie,
                                },
                            );
                        }
                        assertDocArrExpectedFuzzy(
                            stripScores(enrichedExpected),
                            stripScores(resultDoc.enriched_movies),
                        );

                        // Action view results must match ground truth.
                        assert.eq(
                            resultDoc.action_movies.length,
                            actionExpected.length,
                            "action_movies count mismatch",
                            {
                                resultDoc,
                            },
                        );
                        assertDocArrExpectedFuzzy(
                            stripScores(actionExpected),
                            stripScores(resultDoc.action_movies),
                        );

                        // The two result sets are DIFFERENT views; they may overlap (both are
                        // subsets of the movies corpus) but the enriched results include
                        // non-action movies while action results exclude them.  We assert that
                        // action_movies does NOT include movie _id=4 (King Kong, Adventure).
                        const actionIds = resultDoc.action_movies.map((d) => d._id);
                        assert(
                            !actionIds.includes(4),
                            "action_movies in dual-lookup includes non-action movie _id=4",
                            {
                                resultDoc,
                            },
                        );
                    }
                },
            );
        });
    });
});
