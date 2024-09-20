/**
 * Tests hybrid search with rank fusion using verbose syntax without the $rankFusion
 * stage. The collection used in this test includes no search score ties.
 */

import {
    buildExpectedResults,
    getMovieData,
    getPlotEmbeddingById,
    getVectorSearchIndexSpec
} from "jstests/with_mongot/e2e/lib/hybrid_scoring_data.js";
import {assertDocArrExpectedFuzzy} from "jstests/with_mongot/e2e/lib/search_e2e_utils.js";

const collName = "search_rank_fusion";
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));

// Index is blocking by default so that the query is only run after index has been made.
coll.createSearchIndex({name: "search_movie_block", definition: {"mappings": {"dynamic": true}}});

// Create vector search index on movie plot embeddings.
coll.createSearchIndex(getVectorSearchIndexSpec());

const limit = 20;
// Multiplication factor of limit for numCandidates in $vectorSearch.
const vectorSearchOverrequestFactor = 10;

function getSearchPipeline(fullTextSearchConstant) {
    let searchPipeline = [
        {
            $search: {
                index: "search_movie_block",
                text: {query: "ape", path: ["fullplot", "title"]},
            }
        },
        {$limit: limit},
        {$group: {_id: null, docs: {$push: "$$ROOT"}}},
        {$unwind: {path: "$docs", includeArrayIndex: "fts_rank"}},
        {
            $addFields: {
                // RRF: 1 divided by rank + full text search rank constant.
                fts_score: {$divide: [1.0, {$add: ["$fts_rank", fullTextSearchConstant]}]}
            }
        },
        {
            $project: {
                _id: "$docs._id",
                title: "$docs.title",
                fullplot: "$docs.fullplot",
                genres: "$docs.genres",
                plot_embedding: "$docs.plot_embedding",
                fts_score: 1
            }
        }
    ];
    return searchPipeline;
}

function getVectorSearchPipeline(vectorSearchConstant) {
    let vectorSearchPipeline = [
        {
            $vectorSearch: {
                queryVector: getPlotEmbeddingById(6),  //'Tarzan the Ape Man': _id = 6
                path: "plot_embedding",
                numCandidates: limit * vectorSearchOverrequestFactor,
                index: "vector_search_movie_block",
                limit: limit,
            }
        },
        {$limit: limit},
        {$group: {_id: null, docs: {$push: "$$ROOT"}}},
        {$unwind: {path: "$docs", includeArrayIndex: "vs_rank"}},
        {
            $addFields: {
                vs_score: {
                    $divide: [
                        1.0,
                        {
                            $add: [
                                "$vs_rank",
                                vectorSearchConstant
                            ]  // RRF: 1 divided by rank + vector search constant
                        }
                    ]
                }
            }
        },
        {
            $project: {
                _id: "$docs._id",
                title: "$docs.title",
                fullplot: "$docs.fullplot",
                genres: "$docs.genres",
                plot_embedding: "$docs.plot_embedding",
                vs_score: 1
            }
        }
    ];
    return vectorSearchPipeline;
}

let hybridSearchProcessingPipeline = [
    {
        $group: {
            _id: "$_id",
            vs_score: {$max: "$vs_score"},
            fts_score: {$max: "$fts_score"},
            title: {$first: "$title"},
            genres: {$first: "$genres"},
            plot_embedding: {$first: "$plot_embedding"},
            fullplot: {$first: "$fullplot"}
        }
    },
    {
        $project: {
            _id: 1,
            title: 1,
            vs_score: {$ifNull: ["$vs_score", 0]},
            fts_score: {$ifNull: ["$fts_score", 0]},
            fullplot: 1,
            genres: 1,
            plot_embedding: 1
        }
    },
    {$addFields: {score: {$add: ["$fts_score", "$vs_score"]}}},
    {$sort: {score: -1, _id: 1}},
    {$limit: limit},
    {$project: {_id: 1, title: 1, fullplot: 1, genres: 1, plot_embedding: 1}}
];

// Perform a hybrid search with a $vectorSearch on plot_embedding for the plot_embedding of
// and a 'Tarzan the Ape Man' $search on fullplot and title for the keyword "ape"
// Note: In rank fusion a higher rank constant will result in downplaying those results.
function runTest(vectorSearchConstant, fullTextSearchConstant, expectedResultIds) {
    let unionWithSearch = [
        {
            $unionWith: {
                coll: collName,
                pipeline: getSearchPipeline(fullTextSearchConstant),
            }
        },

    ];
    let hybridSearchQuery = getVectorSearchPipeline(vectorSearchConstant)
                                .concat(unionWithSearch)
                                .concat(hybridSearchProcessingPipeline);
    let results = coll.aggregate(hybridSearchQuery).toArray();

    assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds), results);
}

// Perform a hybrid search with $search on fullplot and title for the keyword "ape"
// and a $vectorSearch on plot_embedding for the plot_embedding of 'Tarzan the Ape Man'.
// Note: In rank fusion a higher rank constant will result in downplaying those results.
function runTestFlipped(vectorSearchConstant, fullTextSearchConstant, expectedResultIds) {
    let unionWithVectorSearch = [{
        $unionWith: {
            coll: collName,
            pipeline: getVectorSearchPipeline(vectorSearchConstant),
        }
    }];
    let hybridSearchQuery = getSearchPipeline(fullTextSearchConstant)
                                .concat(unionWithVectorSearch)
                                .concat(hybridSearchProcessingPipeline);
    let results = coll.aggregate(hybridSearchQuery).toArray();

    assert.eq(results, buildExpectedResults(expectedResultIds));
}

// The default rank constants for reciprocal rank fusion is 1.
runTest(/*vectorSearchConstant*/ 1,
        /*fullTextSearchConstant*/ 1,
        /*expectedResultIds*/[6, 4, 1, 2, 3, 8, 5, 9, 10, 12, 13, 14, 11, 7, 15]);

runTestFlipped(/*vectorSearchConstant*/ 1,
               /*fullTextSearchConstant*/ 1,
               /*expectedResultIds*/[6, 4, 1, 2, 3, 8, 5, 9, 10, 12, 13, 14, 11, 7, 15]);

// Customize the rank constants to penalize full text search.
runTest(/*vectorSearchConstant*/ 2,
        /*fullTextSearchConstant*/ 5,
        /*expectedResultIds*/[6, 4, 1, 8, 2, 5, 3, 9, 10, 12, 13, 14, 11, 7, 15]);

runTestFlipped(/*vectorSearchConstant*/ 2,
               /*fullTextSearchConstant*/ 5,
               /*expectedResultIds*/[6, 4, 1, 8, 2, 5, 3, 9, 10, 12, 13, 14, 11, 7, 15]);
