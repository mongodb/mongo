/**
 * Tests hybrid search with rank fusion using verbose syntax without the $rankFusion
 * stage. The collection used in this test includes no search score ties.
 *
 * @tags: [featureFlagRankFusionFull, requires_fcv_81]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieSearchIndexSpec,
    getMovieVectorSearchIndexSpec
} from "jstests/with_mongot/e2e/lib/data/movies.js";
import {
    assertDocArrExpectedFuzzy,
    buildExpectedResults,
    datasets,
} from "jstests/with_mongot/e2e/lib/search_e2e_utils.js";

const collName = "search_rank_fusion";
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));

// Index is blocking by default so that the query is only run after index has been made.
createSearchIndex(coll, getMovieSearchIndexSpec());

// Create vector search index on movie plot embeddings.
createSearchIndex(coll, getMovieVectorSearchIndexSpec());

const limit = 20;
// Multiplication factor of limit for numCandidates in $vectorSearch.
const vectorSearchOverrequestFactor = 10;
const kRankConstant = 60;

function getSearchPipeline() {
    let searchPipeline = [
        {
            $search: {
                index: getMovieSearchIndexSpec().name,
                text: {query: "ape", path: ["fullplot", "title"]},
            }
        },
        {$limit: limit},
        {$group: {_id: null, docs: {$push: "$$ROOT"}}},
        {$unwind: {path: "$docs", includeArrayIndex: "fts_rank"}},
        {
            $addFields: {
                // RRF: 1 divided by rank + full text search rank constant.
                fts_score: {$divide: [1.0, {$add: ["$fts_rank", kRankConstant]}]}
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

function getVectorSearchPipeline() {
    let vectorSearchPipeline = [
        {
            $vectorSearch: {
                queryVector: getMoviePlotEmbeddingById(6),  //'Tarzan the Ape Man': _id = 6
                path: "plot_embedding",
                numCandidates: limit * vectorSearchOverrequestFactor,
                index: getMovieVectorSearchIndexSpec().name,
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
                                kRankConstant
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

function getSearchWithSetWindowFieldsPipeline() {
    let searchPipeline = [
        {
            $search: {
                index: getMovieSearchIndexSpec().name,
                text: {query: "ape", path: ["fullplot", "title"]},
            }
        },
        {$limit: limit},
        {
            $setWindowFields:
                {sortBy: {score: {$meta: "searchScore"}}, output: {fts_rank: {$rank: {}}}},
        },
        {
            $addFields: {
                // RRF: 1 divided by rank + full text search rank constant.
                fts_score: {$divide: [1.0, {$add: ["$fts_rank", kRankConstant]}]}
            }
        },
        {$project: {_id: 1, title: 1, fullplot: 1, genres: 1, plot_embedding: 1, fts_score: 1}}
    ];
    return searchPipeline;
}

function getVectorSearchWithSetWindowFieldsPipeline() {
    let vectorSearchPipeline = [
        {
            $vectorSearch: {
                queryVector: getMoviePlotEmbeddingById(6),  //'Tarzan the Ape Man': _id = 6
                path: "plot_embedding",
                numCandidates: limit * vectorSearchOverrequestFactor,
                index: getMovieVectorSearchIndexSpec().name,
                limit: limit,
            }
        },
        {$limit: limit},
        {
            $setWindowFields:
                {sortBy: {score: {$meta: "vectorSearchScore"}}, output: {vs_rank: {$rank: {}}}},
        },
        {
            $addFields: {
                vs_score: {
                    $divide: [
                        1.0,
                        {
                            $add: [
                                "$vs_rank",
                                kRankConstant
                            ]  // RRF: 1 divided by rank + vector search constant
                        }
                    ]
                }
            }
        },
        {$project: {_id: 1, title: 1, fullplot: 1, genres: 1, plot_embedding: 1, vs_score: 1}}
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
function runTest(expectedResultIds, searchPipeline, vectorSearchPipeline) {
    let unionWithSearch = [
        {
            $unionWith: {
                coll: collName,
                pipeline: searchPipeline,
            }
        },

    ];
    let hybridSearchQuery =
        vectorSearchPipeline.concat(unionWithSearch).concat(hybridSearchProcessingPipeline);
    let results = coll.aggregate(hybridSearchQuery).toArray();

    assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.MOVIES), results);
}

// Perform a hybrid search with $search on fullplot and title for the keyword "ape"
// and a $vectorSearch on plot_embedding for the plot_embedding of 'Tarzan the Ape Man'.
// Note: In rank fusion a higher rank constant will result in downplaying those results.
function runTestFlipped(expectedResultIds, searchPipeline, vectorSearchPipeline) {
    let unionWithVectorSearch = [{
        $unionWith: {
            coll: collName,
            pipeline: vectorSearchPipeline,
        }
    }];
    let hybridSearchQuery =
        searchPipeline.concat(unionWithVectorSearch).concat(hybridSearchProcessingPipeline);
    let results = coll.aggregate(hybridSearchQuery).toArray();

    assert.eq(results, buildExpectedResults(expectedResultIds, datasets.MOVIES));
}

const expectedResultIdOrder = [6, 4, 1, 5, 2, 3, 8, 9, 10, 12, 13, 14, 11, 7, 15];

// Run tests with search in $unionWith
runTest(expectedResultIdOrder, getSearchPipeline(), getVectorSearchPipeline());
runTest(expectedResultIdOrder,
        getSearchWithSetWindowFieldsPipeline(),
        getVectorSearchWithSetWindowFieldsPipeline());

// Run tests with vectorSearch in $unionwith
runTestFlipped(expectedResultIdOrder, getSearchPipeline(), getVectorSearchPipeline());
runTestFlipped(expectedResultIdOrder,
               getSearchWithSetWindowFieldsPipeline(),
               getVectorSearchWithSetWindowFieldsPipeline());
dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});
