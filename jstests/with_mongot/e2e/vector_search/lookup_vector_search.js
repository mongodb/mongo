/**
 * Tests $vectorSearch inside a $lookup subpipeline.
 *
 * @tags: [ requires_fcv_90 ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieVectorSearchIndexSpec,
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {
    assertDocArrExpectedFuzzy,
    stripScores,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const moviesCollName = jsTestName() + "_movies";
const moviesColl = db.getCollection(moviesCollName);

const localCollName = jsTestName() + "_local";
const localColl = db.getCollection(localCollName);

const limit = 10;
const vectorSearchOverrequestFactor = 10;

function getVectorSearchStage() {
    return {
        $vectorSearch: {
            queryVector: getMoviePlotEmbeddingById(6), // embedding for 'Tarzan the Ape Man'
            path: "plot_embedding",
            numCandidates: limit * vectorSearchOverrequestFactor,
            index: getMovieVectorSearchIndexSpec().name,
            limit: limit,
        },
    };
}

function getVectorSearchSubPipeline() {
    // Keep '_id' so assertDocArrExpectedFuzzy() can match documents between arrays.
    return [
        getVectorSearchStage(),
        {$project: {_id: 1, title: 1, score: {$meta: "vectorSearchScore"}}},
    ];
}

function makeLookupPipeline() {
    return [
        {
            $lookup: {
                from: moviesCollName,
                pipeline: getVectorSearchSubPipeline(),
                as: "movies",
            },
        },
        {$sort: {_id: 1}},
    ];
}

describe("$vectorSearch in $lookup subpipeline", function () {
    before(function () {
        moviesColl.drop();
        assert.commandWorked(moviesColl.insertMany(getMovieData()));

        // Index is blocking by default so that queries only run after the index has been built.
        createSearchIndex(moviesColl, getMovieVectorSearchIndexSpec());

        localColl.drop();
        assert.commandWorked(
            localColl.insertMany([
                {_id: 100, targetTitle: "Tarzan the Ape Man"},
                {_id: 101, targetTitle: "King Kong"},
            ]),
        );

        // Make sure the shard that is going to execute the $lookup has up-to-date routing info.
        localColl.aggregate([{$lookup: {from: moviesCollName, pipeline: [], as: "out"}}]).toArray();
    });

    after(function () {
        dropSearchIndex(moviesColl, {name: getMovieVectorSearchIndexSpec().name});
        moviesColl.drop();
        localColl.drop();
    });

    it("returns identical, score-sorted $vectorSearch results for every input document", function () {
        // Ground truth: the same $vectorSearch run as a top-level aggregate on the movies
        // collection.
        const expectedMovies = moviesColl.aggregate(getVectorSearchSubPipeline()).toArray();
        assert.eq(
            expectedMovies.length,
            limit,
            "ground truth $vectorSearch returned unexpected count",
            {
                expectedMovies,
            },
        );

        const results = localColl.aggregate(makeLookupPipeline()).toArray();
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
            // The uncorrelated subpipeline must produce the same results (by '_id' + 'title')
            // as the top-level $vectorSearch for every input document.
            assertDocArrExpectedFuzzy(stripScores(expectedMovies), stripScores(resultDoc.movies));
        }
    });

    it("supports a correlated $match with let variables after $vectorSearch", function () {
        // This relies on both target titles appearing in the top-'limit' vector results for
        // Tarzan's embedding: 'Tarzan the Ape Man' (id 6) is rank-1 by construction (the query
        // vector is its own embedding), while 'King Kong' (id 4) is empirically within the top
        // 10 of this small ape/jungle-heavy movie corpus.
        const results = localColl
            .aggregate([
                {
                    $lookup: {
                        from: moviesCollName,
                        let: {wantedTitle: "$targetTitle"},
                        pipeline: getVectorSearchSubPipeline().concat([
                            {$match: {$expr: {$eq: ["$title", "$$wantedTitle"]}}},
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
                "expected exactly one movie matching the let variable",
                {
                    resultDoc,
                },
            );
            assert.eq(
                resultDoc.movies[0].title,
                resultDoc.targetTitle,
                "correlated $match returned wrong movie",
                {
                    resultDoc,
                },
            );
        }
    });

    it("rejects $vectorSearch in a $lookup subpipeline against a timeseries collection", function () {
        const tsCollName = jsTestName() + "_timeseries";
        db.runCommand({drop: tsCollName});
        assert.commandWorked(
            db.createCollection(tsCollName, {timeseries: {timeField: "t", metaField: "m"}}),
        );
        try {
            assert.commandFailedWithCode(
                db.runCommand({
                    aggregate: localCollName,
                    pipeline: [
                        {
                            $lookup: {
                                from: tsCollName,
                                pipeline: getVectorSearchSubPipeline(),
                                as: "movies",
                            },
                        },
                    ],
                    cursor: {},
                }),
                [12093200, 10557302],
            );
        } finally {
            db.getCollection(tsCollName).drop();
        }
    });

    it("rejects $vectorSearch that is not the first stage of the subpipeline with error 40602", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: localCollName,
                pipeline: [
                    {
                        $lookup: {
                            from: moviesCollName,
                            pipeline: [{$limit: 1}].concat(getVectorSearchSubPipeline()),
                            as: "movies",
                        },
                    },
                ],
                cursor: {},
            }),
            40602,
        );
    });
});
