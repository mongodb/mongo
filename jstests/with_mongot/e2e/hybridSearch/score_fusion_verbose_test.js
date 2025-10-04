/**
 * Tests hybrid search with relative score fusion using the syntax before the $scoreFusion
 * aggregation stage is introduced.
 *
 * This file runs the same hybrid search with two different score normalization methods:
 * Sigmoid and MinMaxScaler.
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieSearchIndexSpec,
    getMovieVectorSearchIndexSpec,
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {
    assertDocArrExpectedFuzzy,
    buildExpectedResults,
    datasets,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));

// Index is blocking by default so that the query is only run after index has been made.
createSearchIndex(coll, getMovieSearchIndexSpec());

// Create vector search index on movie plot embeddings.
createSearchIndex(coll, getMovieVectorSearchIndexSpec());

function runQueryTest(query, expectedResults) {
    let results = coll.aggregate(query).toArray();
    assertDocArrExpectedFuzzy(expectedResults, results);
}

const limit = 20;
// Multiplication factor of limit for numCandidates in $vectorSearch.
const vectorSearchOverrequestFactor = 10;

// Common query components accross different hybrid search queries.
let vectorSearchPipeline = [
    {
        $vectorSearch: {
            queryVector: getMoviePlotEmbeddingById(6), // get the embedding for 'Tarzan the Ape Man', which has _id = 6
            path: "plot_embedding",
            numCandidates: limit * vectorSearchOverrequestFactor,
            index: getMovieVectorSearchIndexSpec().name,
            limit: 2 * limit,
        },
    },
    {$addFields: {vs_score: {$meta: "vectorSearchScore"}}},
];

let fullTextSearchPipeline = [
    // This search term 'ape' is related to the vector search for 'Tarzan the Ape Man'.
    {
        $search: {
            index: getMovieSearchIndexSpec().name,
            text: {query: "ape", path: ["fullplot", "title"]},
        },
    },
    {$limit: 2 * limit},
    {$addFields: {fts_score: {$meta: "searchScore"}}},
];

let mixVsFtsResultsPipeline = [
    {
        // These fields must be grouped in the exactly correct order,
        // or the equality assertion on the results will fail.
        $group: {
            _id: "$_id",
            title: {$first: "$title"},
            genres: {$first: "$genres"},
            plot_embedding: {$first: "$plot_embedding"},
            fullplot: {$first: "$fullplot"},
            vs_score: {$max: "$vs_score"},
            fts_score: {$max: "$fts_score"},
        },
    },
    {
        $project: {
            _id: 1,
            title: 1,
            fullplot: 1,
            genres: 1,
            plot_embedding: 1,
            vs_score: {$ifNull: ["$vs_score", 0]},
            fts_score: {$ifNull: ["$fts_score", 0]},
        },
    },
    {$addFields: {score: {$add: ["$fts_score", "$vs_score"]}}},
    {$sort: {score: -1, _id: 1}},
    {$limit: limit},
    {$project: {_id: 1, title: 1, fullplot: 1, genres: 1, plot_embedding: 1}},
];

// Test 1: Sigmoid score normalization
runQueryTest(
    vectorSearchPipeline
        .concat([
            {
                $project: {
                    _id: 1,
                    title: 1,
                    fullplot: 1,
                    genres: 1,
                    plot_embedding: 1,
                    vs_score: {
                        $multiply: [1, {$divide: [1, {$sum: [1, {$exp: {$multiply: [-1, "$vs_score"]}}]}]}],
                    },
                },
            },
            {
                $unionWith: {
                    coll: collName,
                    pipeline: fullTextSearchPipeline.concat([
                        {
                            $project: {
                                _id: 1,
                                title: 1,
                                fullplot: 1,
                                genres: 1,
                                plot_embedding: 1,
                                fts_score: {
                                    $multiply: [
                                        1,
                                        {
                                            $divide: [1, {$sum: [1, {$exp: {$multiply: [-1, "$fts_score"]}}]}],
                                        },
                                    ],
                                },
                            },
                        },
                    ]),
                },
            },
        ])
        .concat(mixVsFtsResultsPipeline),
    buildExpectedResults(/*expectedResultIds*/ [6, 1, 2, 3, 4, 5, 8, 9, 10, 12, 13, 14, 11, 7, 15], datasets.MOVIES),
);

// Test 2: MinMaxScaler score normalization
runQueryTest(
    vectorSearchPipeline
        .concat([
            {
                $setWindowFields: {output: {min_vs_score: {$min: "$vs_score"}, max_vs_score: {$max: "$vs_score"}}},
            },
            {
                $project: {
                    _id: 1,
                    title: 1,
                    fullplot: 1,
                    genres: 1,
                    plot_embedding: 1,
                    vs_score: {
                        $divide: [
                            {$subtract: ["$vs_score", "$min_vs_score"]},
                            {$subtract: ["$max_vs_score", "$min_vs_score"]},
                        ],
                    },
                },
            },
            {
                $unionWith: {
                    coll: collName,
                    pipeline: fullTextSearchPipeline.concat([
                        {
                            $setWindowFields: {
                                output: {
                                    min_fts_score: {$min: "$fts_score"},
                                    max_fts_score: {$max: "$fts_score"},
                                },
                            },
                        },
                        {
                            $project: {
                                _id: 1,
                                title: 1,
                                fullplot: 1,
                                genres: 1,
                                plot_embedding: 1,
                                fts_score: {
                                    $divide: [
                                        {$subtract: ["$fts_score", "$min_fts_score"]},
                                        {$subtract: ["$max_fts_score", "$min_fts_score"]},
                                    ],
                                },
                            },
                        },
                    ]),
                },
            },
        ])
        .concat(mixVsFtsResultsPipeline),
    buildExpectedResults(/*expectedResultIds*/ [6, 1, 4, 2, 8, 9, 10, 3, 12, 13, 5, 14, 11, 7, 15], datasets.MOVIES),
);

dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});
