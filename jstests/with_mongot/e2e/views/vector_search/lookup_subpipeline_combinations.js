/**
 * Tests additional subpipeline-combination shapes for $vectorSearch in $lookup targeting
 * mongot-indexed views: $unionWith inside subpipelines, nested $lookups inside $unionWith,
 * returnStoredSource failures (expected to be consistent with top-level), and pre-filtered
 * $vectorSearch. Ground-truth oracle: top-level $vectorSearch on the view.
 *
 * @tags: [
 *   featureFlagMongotIndexedViews,
 *   featureFlagExtensionsInsideHybridSearch,
 *   requires_fcv_81,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";
import {
    createMoviesViewAndIndex,
    getMoviePlotEmbeddingById,
    getMovieVectorSearchIndexSpec,
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {
    assertDocArrExpectedFuzzy,
    datasets,
    stripScores,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

// -------------------------------------------------------------------------
// Shared view handles.
// -------------------------------------------------------------------------
const enrichedView = createMoviesViewAndIndex(datasets.MOVIES_WITH_ENRICHED_TITLE);
const actionView = createMoviesViewAndIndex(datasets.ACTION_MOVIES);

const viewDb = enrichedView.getDB();

const kLimit = 5;

/**
 * Builds a $vectorSearch stage targeting the given index/limit.
 * queryVector is always movie _id=6's embedding ("Tarzan the Ape Man").
 */
function makeVectorSearchStage(indexName, limit = kLimit, extraOpts = {}) {
    return {
        $vectorSearch: Object.assign(
            {
                queryVector: getMoviePlotEmbeddingById(6),
                path: "plot_embedding",
                exact: true,
                index: indexName,
                limit: limit,
            },
            extraOpts,
        ),
    };
}

/** Subpipeline for use inside $lookup: [$vectorSearch, $project]. */
function makeSubPipeline(indexName, limit = kLimit, extraOpts = {}) {
    return [
        makeVectorSearchStage(indexName, limit, extraOpts),
        {$project: {_id: 1, title: 1, enriched_title: 1, score: {$meta: "vectorSearchScore"}}},
    ];
}

// -------------------------------------------------------------------------
// Index with a filter field to support the filtered $vectorSearch test.
// We create a per-test index with the _id field as a filter to support
// pre-filtering by _id.
// -------------------------------------------------------------------------
const kFilteredIndexName = jsTestName() + "_filtered_idx";

describe("$vectorSearch in $lookup subpipeline: $unionWith combinations, returnStoredSource, and filter", function () {
    const localCollName = jsTestName() + "_local";
    const localColl = viewDb.getCollection(localCollName);

    before(function () {
        localColl.drop();
        assert.commandWorked(
            localColl.insertMany([
                {_id: 600, label: "doc_a"},
                {_id: 601, label: "doc_b"},
            ]),
        );

        // Create a filtered vector index on enrichedView to support the filtered $vectorSearch test.
        // The filter field is _id (numeric), allowing pre-filtering by _id range.
        const filteredIndexSpec = getMovieVectorSearchIndexSpec({
            indexName: kFilteredIndexName,
            filterFields: [{type: "filter", path: "_id"}],
        });
        const existingIndexes = enrichedView.aggregate([{$listSearchIndexes: {}}]).toArray();
        if (!existingIndexes.some((idx) => idx.name === kFilteredIndexName)) {
            createSearchIndex(enrichedView, filteredIndexSpec);
        }

        // Prime routing info.
        localColl
            .aggregate([{$lookup: {from: enrichedView.getName(), pipeline: [], as: "out"}}])
            .toArray();
        localColl
            .aggregate([{$lookup: {from: actionView.getName(), pipeline: [], as: "out"}}])
            .toArray();
    });

    after(function () {
        localColl.drop();
        // Drop the per-test filtered index (private to this file).
        try {
            dropSearchIndex(enrichedView, {name: kFilteredIndexName});
        } catch (e) {
            // Best effort — don't fail cleanup.
            jsTest.log.info("best-effort index drop failed", {error: e});
        }
    });

    describe("$unionWith inside a $lookup subpipeline targeting two views", function () {
        it("returns combined results from both views consistent with their top-level ground truths", function () {
            // Ground truths.
            const enrichedExpected = enrichedView
                .aggregate(makeSubPipeline(datasets.MOVIES_WITH_ENRICHED_TITLE.indexName))
                .toArray();
            const actionExpected = actionView
                .aggregate(makeSubPipeline(datasets.ACTION_MOVIES.indexName))
                .toArray();

            assert.gt(enrichedExpected.length, 0, "enriched ground truth empty");
            assert.gt(actionExpected.length, 0, "action ground truth empty");

            // Subpipeline: $vectorSearch on enrichedView + $unionWith on actionView.
            const subPipeline = [
                makeVectorSearchStage(datasets.MOVIES_WITH_ENRICHED_TITLE.indexName),
                {
                    $project: {
                        _id: 1,
                        title: 1,
                        enriched_title: 1,
                        score: {$meta: "vectorSearchScore"},
                    },
                },
                {
                    $unionWith: {
                        coll: actionView.getName(),
                        pipeline: makeSubPipeline(datasets.ACTION_MOVIES.indexName),
                    },
                },
            ];

            const results = localColl
                .aggregate([
                    {
                        $lookup: {
                            from: enrichedView.getName(),
                            pipeline: subPipeline,
                            as: "combined",
                        },
                    },
                    {$sort: {_id: 1}},
                ])
                .toArray();

            assert.eq(results.length, 2, "expected one result per local doc", {results});

            const expectedTotal = enrichedExpected.length + actionExpected.length;

            for (const resultDoc of results) {
                assert.eq(
                    resultDoc.combined.length,
                    expectedTotal,
                    "combined lookup+unionWith result count mismatch",
                    {
                        resultDoc,
                    },
                );

                // NOTE: the sibling whole-array pattern (unionWith.js) cannot be used here
                // because assertDocArrExpectedFuzzy forbids duplicate '_id's in the expected
                // array, and the enriched and action result sets overlap (both contain
                // movies 6, 9, 10 for this query vector).  Instead, exploit the
                // deterministic $unionWith ordering: the subpipeline's own results come
                // first, followed by the unioned results, so the slice boundary at
                // enrichedExpected.length cleanly separates the two parts.
                const enrichedPart = resultDoc.combined.slice(0, enrichedExpected.length);
                const actionPart = resultDoc.combined.slice(enrichedExpected.length);

                assertDocArrExpectedFuzzy(stripScores(enrichedExpected), stripScores(enrichedPart));
                assertDocArrExpectedFuzzy(stripScores(actionExpected), stripScores(actionPart));
            }
        });
    });

    describe("$lookup-with-$vectorSearch-on-view inside a $unionWith subpipeline", function () {
        it("correctly resolves a $lookup from a view with $vectorSearch nested inside $unionWith", function () {
            // Ground truth for the inner $lookup.
            const innerExpected = enrichedView
                .aggregate(makeSubPipeline(datasets.MOVIES_WITH_ENRICHED_TITLE.indexName))
                .toArray();
            assert.gt(innerExpected.length, 0, "inner ground truth empty");

            // localColl has 2 docs. The $unionWith runs on the same localColl but
            // with a $lookup subpipeline, so it produces 2 joined docs added to the
            // outer stream (total 4 docs, but the $unionWith docs have movies arrays).
            const pipeline = [
                // Outer part: just project _id from localColl.
                {$project: {_id: 1, label: 1}},
                // $unionWith part: re-read localColl, each doc gets a "movies" array
                // via $lookup from enrichedView with $vectorSearch.
                {
                    $unionWith: {
                        coll: localCollName,
                        pipeline: [
                            {
                                $lookup: {
                                    from: enrichedView.getName(),
                                    pipeline: makeSubPipeline(
                                        datasets.MOVIES_WITH_ENRICHED_TITLE.indexName,
                                    ),
                                    as: "movies",
                                },
                            },
                        ],
                    },
                },
            ];

            const results = localColl.aggregate(pipeline).toArray();

            // 2 docs from outer $project + 2 docs from $unionWith (each with movies).
            assert.eq(results.length, 4, "expected 4 docs (2 outer + 2 unionWith)", {results});

            // The first 2 docs (from outer $project) have no movies array.
            const outerDocs = results.slice(0, 2);
            for (const d of outerDocs) {
                assert(!d.hasOwnProperty("movies"), "outer doc should not have movies", {d});
            }

            // The last 2 docs (from $unionWith) each have a movies array.
            const unionWithDocs = results.slice(2);
            for (const d of unionWithDocs) {
                assert(d.hasOwnProperty("movies"), "$unionWith doc missing movies array", {d});
                assert.eq(
                    d.movies.length,
                    innerExpected.length,
                    "movies array in $unionWith doc has wrong count",
                    {d},
                );

                assertDocArrExpectedFuzzy(stripScores(innerExpected), stripScores(d.movies));
            }
        });
    });

    describe("returnStoredSource: true in $vectorSearch in $lookup on a view", function () {
        it("fails consistently with top-level when storedSource is not configured on the index", function () {
            const rsTrue = {returnStoredSource: true};
            const subPipeline = makeSubPipeline(
                datasets.MOVIES_WITH_ENRICHED_TITLE.indexName,
                kLimit,
                rsTrue,
            );

            // Ground truth: top-level also fails (storedSource not configured).
            const topLevelErr = assert.throws(
                () => {
                    enrichedView.aggregate(subPipeline).toArray();
                },
                [],
                "expected top-level returnStoredSource to fail when storedSource not configured",
            );

            // $lookup must fail the same way.
            const lookupErr = assert.throws(
                () => {
                    localColl
                        .aggregate([
                            {
                                $lookup: {
                                    from: enrichedView.getName(),
                                    pipeline: subPipeline,
                                    as: "movies",
                                },
                            },
                        ])
                        .toArray();
                },
                [],
                "expected $lookup with returnStoredSource to fail when storedSource not configured",
            );

            // Both runs must surface the SAME error code.
            assert.eq(
                topLevelErr.code,
                lookupErr.code,
                "top-level and $lookup returnStoredSource failures should have the same error code",
                {topLevelErr, lookupErr},
            );

            // Both errors must reference "storedSource is not configured".
            const topLevelMsg = topLevelErr.message || topLevelErr.errmsg || String(topLevelErr);
            const lookupMsg = lookupErr.message || lookupErr.errmsg || String(lookupErr);
            assert(
                topLevelMsg.includes("storedSource is not configured"),
                "top-level error should mention storedSource not configured",
                {topLevelMsg},
            );
            assert(
                lookupMsg.includes("storedSource is not configured"),
                "$lookup error should mention storedSource not configured (consistent with top-level)",
                {lookupMsg},
            );
        });
    });

    describe("$vectorSearch with a filter in $lookup on a view", function () {
        it("returns only filtered results, consistent with top-level filtered $vectorSearch", function () {
            const filterSpec = {filter: {_id: {$gte: 6}}};
            const subPipeline = makeSubPipeline(kFilteredIndexName, kLimit, filterSpec);

            // Ground truth: top-level on the view with the same filter.
            const expected = enrichedView.aggregate(subPipeline).toArray();
            assert.gt(
                expected.length,
                0,
                "filtered $vectorSearch ground truth returned no results",
            );

            // All ground-truth results must satisfy the filter.
            for (const doc of expected) {
                assert.gte(doc._id, 6, "ground truth doc violates filter _id >= 6", {doc});
            }

            // Run via $lookup.
            const results = localColl
                .aggregate([
                    {
                        $lookup: {
                            from: enrichedView.getName(),
                            pipeline: subPipeline,
                            as: "movies",
                        },
                    },
                    {$sort: {_id: 1}},
                ])
                .toArray();

            assert.eq(results.length, 2, "expected one result per local doc", {results});

            for (const resultDoc of results) {
                assert.eq(
                    resultDoc.movies.length,
                    expected.length,
                    "filtered lookup result count mismatch",
                    {
                        resultDoc,
                    },
                );

                // All results must satisfy the filter.
                for (const movie of resultDoc.movies) {
                    assert.gte(movie._id, 6, "filtered lookup result violates _id >= 6 filter", {
                        movie,
                    });
                }

                assertDocArrExpectedFuzzy(stripScores(expected), stripScores(resultDoc.movies));
            }
        });
    });
});
