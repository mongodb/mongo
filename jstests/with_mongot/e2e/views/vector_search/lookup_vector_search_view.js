/**
 * Tests $vectorSearch inside a $lookup subpipeline when the foreign collection is a
 * mongot-indexed view.
 *
 * The view adds an `enriched_title` field via $addFields. Each test case asserts that:
 *  - view-transform fields are present in the joined documents (idLookup applied the view
 *    transforms), AND
 *  - the $vectorSearch results match a ground-truth top-level aggregate on the view.
 *
 * @tags: [
 *   featureFlagMongotIndexedViews,
 *   featureFlagExtensionsInsideHybridSearch,
 *   requires_fcv_81,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    createMoviesViewAndIndex,
    getMoviePlotEmbeddingById,
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {
    assertDocArrExpectedFuzzy,
    datasets,
    stripScores,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

// The view + its vector index are created once for the whole describe block via the shared
// movies helper (vector_search_shared_db).  We grab a handle here so the name is known.
const moviesView = createMoviesViewAndIndex(datasets.MOVIES_WITH_ENRICHED_TITLE);
const moviesViewName = moviesView.getName();

// A small local collection used as the outer side of $lookup. It must live in the same database as
// the view, because $lookup resolves the bare 'from' name against the outer aggregation's database
// (the view is created in 'vector_search_shared_db' by the shared movies helper).
const viewDb = moviesView.getDB();
const localCollName = jsTestName() + "_local";
const localColl = viewDb.getCollection(localCollName);

const limit = 5;

/**
 * Returns the $vectorSearch stage that targets the view's vector index.
 */
function getViewVectorSearchStage() {
    return {
        $vectorSearch: {
            queryVector: getMoviePlotEmbeddingById(6), // embedding for 'Tarzan the Ape Man'
            path: "plot_embedding",
            exact: true,
            index: datasets.MOVIES_WITH_ENRICHED_TITLE.indexName,
            limit: limit,
        },
    };
}

/**
 * Returns a subpipeline for use inside $lookup: [$vectorSearch, $project].
 * '_id' is kept so assertDocArrExpectedFuzzy() can correlate documents.
 */
function getViewVectorSearchSubPipeline() {
    return [
        getViewVectorSearchStage(),
        {
            $project: {
                _id: 1,
                title: 1,
                enriched_title: 1,
                score: {$meta: "vectorSearchScore"},
            },
        },
    ];
}

describe("$vectorSearch in $lookup subpipeline where the foreign collection is a mongot-indexed view", function () {
    before(function () {
        // Populate the local (outer) collection. Each doc targets the enriched form
        // (as produced by the view's $addFields: "<id> - <title> (<genres[0]>)") of a
        // movie that reliably appears in the top-'limit' exact vector results for
        // movie 6's embedding: movie 6 is rank-1 by construction (the query vector is
        // its own embedding) and movie 4 ('King Kong') is in the deterministic exact
        // top-5 [6, 4, 8, 9, 10] for this corpus.
        localColl.drop();
        assert.commandWorked(
            localColl.insertMany([
                {_id: 200, targetEnrichedTitle: "6 - Tarzan the Ape Man (Action)"},
                {_id: 201, targetEnrichedTitle: "4 - King Kong (Adventure)"},
            ]),
        );

        // Prime routing info so $lookup can resolve the view's namespace.
        localColl.aggregate([{$lookup: {from: moviesViewName, pipeline: [], as: "out"}}]).toArray();
    });

    after(function () {
        // The shared movies view + vector index in vector_search_shared_db are
        // intentionally left in place: sibling tests in this suite reuse them via
        // checkForExistingIndex().
        localColl.drop();
    });

    it("returns $vectorSearch results that match ground truth and include the view-transform field", function () {
        // Ground truth: the same $vectorSearch run as a top-level aggregate on the
        // view.
        const expectedMovies = moviesView.aggregate(getViewVectorSearchSubPipeline()).toArray();
        assert.eq(
            expectedMovies.length,
            limit,
            "ground truth $vectorSearch on view returned unexpected count",
            {
                expectedMovies,
            },
        );

        // Every ground-truth document must carry the view-transform field.
        for (const doc of expectedMovies) {
            assert(
                doc.hasOwnProperty("enriched_title"),
                "ground-truth doc missing enriched_title (view transform not applied)",
                {doc},
            );
        }

        // Run $lookup with $vectorSearch subpipeline targeting the view.
        const results = localColl
            .aggregate([
                {
                    $lookup: {
                        from: moviesViewName,
                        pipeline: getViewVectorSearchSubPipeline(),
                        as: "movies",
                    },
                },
                {$sort: {_id: 1}},
            ])
            .toArray();

        assert.eq(results.length, 2, "expected one result per local document", {results});

        for (const resultDoc of results) {
            assert.eq(resultDoc.movies.length, limit, "expected 'limit' movies in the 'as' array", {
                resultDoc,
            });

            // Scores must be sorted descending.
            for (let i = 1; i < resultDoc.movies.length; i++) {
                assert.lte(
                    resultDoc.movies[i].score,
                    resultDoc.movies[i - 1].score,
                    "scores not sorted descending",
                    {
                        resultDoc,
                    },
                );
            }

            // Each joined document must carry the view-transform field,
            // proving that idLookup applied the view pipeline.
            for (const movieDoc of resultDoc.movies) {
                assert(
                    movieDoc.hasOwnProperty("enriched_title"),
                    "joined doc missing enriched_title — view transforms were NOT applied",
                    {movieDoc},
                );
            }

            // Results (by '_id' + 'title') must match ground truth.
            assertDocArrExpectedFuzzy(stripScores(expectedMovies), stripScores(resultDoc.movies));
        }
    });

    it("supports a correlated let + $match after $vectorSearch on the view", function () {
        // Each local doc's 'targetEnrichedTitle' is matched, via a let-bound
        // variable, against the view-added 'enriched_title' field of the
        // $vectorSearch results. This proves both correlated execution and that
        // the view transform's output is visible to the correlated $match.
        const results = localColl
            .aggregate([
                {
                    $lookup: {
                        from: moviesViewName,
                        let: {wanted: "$targetEnrichedTitle"},
                        pipeline: getViewVectorSearchSubPipeline().concat([
                            {$match: {$expr: {$eq: ["$enriched_title", "$$wanted"]}}},
                        ]),
                        as: "movies",
                    },
                },
                {$sort: {_id: 1}},
            ])
            .toArray();

        assert.eq(results.length, 2, "expected one result per local document", {results});

        for (const resultDoc of results) {
            assert.eq(
                resultDoc.movies.length,
                1,
                "expected exactly one movie matching the let-bound enriched title",
                {
                    resultDoc,
                },
            );

            const joinedMovie = resultDoc.movies[0];

            // The joined movie's view-added field must equal the let-bound value
            // from this specific local doc.
            assert.eq(
                joinedMovie.enriched_title,
                resultDoc.targetEnrichedTitle,
                "correlated $match returned the wrong movie",
                {resultDoc},
            );
        }
    });
});
