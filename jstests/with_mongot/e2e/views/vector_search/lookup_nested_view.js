/**
 * Tests $vectorSearch inside $lookup subpipelines targeting views: nested views (view-on-a-view),
 * $match-transform views, and correlated subpipelines on view-added fields. Each case compares
 * against the same $vectorSearch run top-level on the view as ground truth to ensure view
 * transformations are correctly applied in subpipelines.
 *
 * @tags: [
 *   featureFlagMongotIndexedViews,
 *   requires_fcv_90,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    createMoviesViewAndIndex,
    getMoviePlotEmbeddingById,
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {
    assertDocArrExpectedFuzzy,
    assertIfVectorSearchNotAllowedInLookup,
    datasets,
    stripScores,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

// -------------------------------------------------------------------------
// Shared view handles — created once, reused across all describe blocks.
// Both views live in vector_search_shared_db (managed by the shared helper).
// -------------------------------------------------------------------------
const nestedView = createMoviesViewAndIndex(datasets.ACTION_MOVIES_WITH_ENRICHED_TITLE);
const matchView = createMoviesViewAndIndex(datasets.ACTION_MOVIES);

const viewDb = nestedView.getDB();

const kLimit = 5;

/**
 * Returns a $vectorSearch stage targeting the given index with a fixed query vector (embedding
 * for movie _id=6, "Tarzan the Ape Man").
 */
function makeVectorSearchStage(indexName) {
    return {
        $vectorSearch: {
            queryVector: getMoviePlotEmbeddingById(6),
            path: "plot_embedding",
            exact: true,
            index: indexName,
            limit: kLimit,
        },
    };
}

/**
 * Returns a subpipeline for $lookup: [$vectorSearch, $project].
 * Keeps _id so assertDocArrExpectedFuzzy can correlate documents.
 */
function makeSubPipeline(indexName, extraFields = {}) {
    return [
        makeVectorSearchStage(indexName),
        {
            $project: Object.assign(
                {_id: 1, title: 1, score: {$meta: "vectorSearchScore"}},
                extraFields,
            ),
        },
    ];
}

// =========================================================================
// Outer describe — shared setup/teardown for all cases.
// =========================================================================
describe("$vectorSearch in $lookup subpipeline: nested view and $match-transform view edge cases", function () {
    // One private outer collection shared by all sub-describes in this file.
    const localCollName = jsTestName() + "_local";
    const localColl = viewDb.getCollection(localCollName);

    before(function () {
        localColl.drop();
        assert.commandWorked(
            localColl.insertMany([
                // These enriched_title values exist on both the nested and the plain enriched
                // view; "Tarzan" (6) appears in both the nested and the action-only view.
                {
                    _id: 300,
                    label: "tarzan_doc",
                    targetEnrichedTitle: "6 - Tarzan the Ape Man (Action)",
                },
                {
                    _id: 301,
                    label: "non_action_doc",
                    targetEnrichedTitle: "4 - King Kong (Adventure)",
                },
            ]),
        );

        // Prime routing info for both foreign namespaces.
        localColl
            .aggregate([{$lookup: {from: nestedView.getName(), pipeline: [], as: "out"}}])
            .toArray();
        localColl
            .aggregate([{$lookup: {from: matchView.getName(), pipeline: [], as: "out"}}])
            .toArray();
    });

    after(function () {
        localColl.drop();
        // Shared views and indexes are left for sibling tests to reuse.
    });

    describe("$lookup from a nested view (view-on-a-view) with $vectorSearch", function () {
        it("returns results consistent with top-level $vectorSearch on the nested view", function () {
            const subPipeline = makeSubPipeline(
                datasets.ACTION_MOVIES_WITH_ENRICHED_TITLE.indexName,
                {
                    enriched_title: 1,
                },
            );

            // Ground truth.
            const expected = nestedView.aggregate(subPipeline).toArray();
            assert.gt(expected.length, 0, "ground-truth query on nested view returned no results", {
                expected,
            });

            // Every ground-truth document must have the view-added field, proving both
            // transformations (addFields + action filter) were applied.
            for (const doc of expected) {
                assert(
                    doc.hasOwnProperty("enriched_title"),
                    "ground-truth doc from nested view missing enriched_title",
                    {doc},
                );
                // All results should be Action movies (the outer $match in the nested view).
                // We can only verify this indirectly via the enriched_title format
                // "<id> - <title> (Action)".
                assert(
                    doc.enriched_title.endsWith("(Action)"),
                    "nested view doc is not an Action movie",
                    {doc},
                );
            }

            // Run via $lookup.
            const pipeline = [
                {
                    $lookup: {
                        from: nestedView.getName(),
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
                            expected.length,
                            "lookup result movie count differs from ground truth",
                            {resultDoc},
                        );

                        // Scores must be sorted descending.
                        for (let i = 1; i < resultDoc.movies.length; i++) {
                            assert.lte(
                                resultDoc.movies[i].score,
                                resultDoc.movies[i - 1].score,
                                "scores not sorted descending in nested-view lookup result",
                                {resultDoc},
                            );
                        }

                        // Each joined doc must carry the view-added enriched_title.
                        for (const movieDoc of resultDoc.movies) {
                            assert(
                                movieDoc.hasOwnProperty("enriched_title"),
                                "joined doc from nested-view lookup missing enriched_title",
                                {movieDoc},
                            );
                            assert(
                                movieDoc.enriched_title.endsWith("(Action)"),
                                "nested-view joined doc is not an Action movie",
                                {movieDoc},
                            );
                        }

                        // Results must match ground truth (ignoring scores).
                        assertDocArrExpectedFuzzy(
                            stripScores(expected),
                            stripScores(resultDoc.movies),
                        );
                    }
                },
            );
        });
    });

    describe("$lookup from a $match-transform view (action filter)", function () {
        it("returns only documents satisfying the view filter, consistent with top-level result", function () {
            const subPipeline = makeSubPipeline(datasets.ACTION_MOVIES.indexName);

            // Ground truth.
            const expected = matchView.aggregate(subPipeline).toArray();
            assert.gt(
                expected.length,
                0,
                "ground-truth query on action-movies view returned no results",
                {
                    expected,
                },
            );

            // Verify ground truth itself excludes non-action movies.  Top-5 for movie-6's
            // embedding on the ACTION_MOVIES view are documented in match_base_case.js as
            // [6, 9, 10, 12, 13] — all Action.  Movie 4 (King Kong, Adventure) must be
            // absent.
            const ids = expected.map((d) => d._id);
            assert(
                !ids.includes(4),
                "ground truth from $match view incorrectly includes non-action movie _id=4",
                {
                    ids,
                },
            );

            // Run via $lookup.
            const pipeline = [
                {
                    $lookup: {
                        from: matchView.getName(),
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
                        // The $match view filters, so there may be fewer than kLimit results.
                        assert.eq(
                            resultDoc.movies.length,
                            expected.length,
                            "lookup result count differs from ground truth",
                            {
                                resultDoc,
                            },
                        );

                        // Non-action movie _id=4 must NOT appear in lookup results either.
                        const resultIds = resultDoc.movies.map((d) => d._id);
                        assert(
                            !resultIds.includes(4),
                            "$lookup result from $match view incorrectly includes non-action movie _id=4",
                            {resultDoc},
                        );

                        assertDocArrExpectedFuzzy(
                            stripScores(expected),
                            stripScores(resultDoc.movies),
                        );
                    }
                },
            );
        });
    });

    describe("Correlated let+$match on the nested view's view-added field", function () {
        it("returns one movie for the action doc and zero for the non-action doc", function () {
            const subPipeline = makeSubPipeline(
                datasets.ACTION_MOVIES_WITH_ENRICHED_TITLE.indexName,
                {
                    enriched_title: 1,
                },
            );

            const pipeline = [
                {
                    $lookup: {
                        from: nestedView.getName(),
                        let: {wanted: "$targetEnrichedTitle"},
                        pipeline: subPipeline.concat([
                            {$match: {$expr: {$eq: ["$enriched_title", "$$wanted"]}}},
                        ]),
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

                    const tarzanDoc = results.find((d) => d._id === 300);
                    const kingKongDoc = results.find((d) => d._id === 301);

                    assert(tarzanDoc, "result for _id=300 (Tarzan) missing", {results});
                    assert(kingKongDoc, "result for _id=301 (King Kong) missing", {results});

                    // Tarzan IS an action movie — should match in the nested view.
                    assert.eq(
                        tarzanDoc.movies.length,
                        1,
                        "expected exactly one matched movie for Tarzan in nested-view correlated lookup",
                        {tarzanDoc},
                    );
                    assert.eq(
                        tarzanDoc.movies[0].enriched_title,
                        tarzanDoc.targetEnrichedTitle,
                        "correlated $match returned wrong movie for Tarzan",
                        {tarzanDoc},
                    );

                    // King Kong is NOT an action movie — the nested view filters it out, so the
                    // correlated $match must return zero results.
                    assert.eq(
                        kingKongDoc.movies.length,
                        0,
                        "expected zero results for non-action movie in nested-view correlated lookup",
                        {kingKongDoc},
                    );
                },
            );
        });
    });
});
