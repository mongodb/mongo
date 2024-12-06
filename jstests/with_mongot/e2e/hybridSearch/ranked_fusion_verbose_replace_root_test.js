/**
 * Tests hybrid search with rank fusion using verbose syntax without the $rankFusion
 * stage and by relying on $replaceRoot rather than individually specifying each field.
 * The collection used in this test includes no search score ties.
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

// Perform a hybrid search with $search on fullplot and title for the keyword "ape"
// and a $vectorSearch on plot_embedding for the plot_embedding of 'Tarzan the Ape Man'.
// Note: In rank fusion a higher rank constant will result in downplaying those results.
function runTest(expectedResultIds) {
    let hybridSearchQuery = [
        {
            $vectorSearch: {
                // Get the embedding for 'Tarzan the Ape Man', which has _id = 6.
                queryVector: getMoviePlotEmbeddingById(6),
                path: "plot_embedding",
                numCandidates: limit * vectorSearchOverrequestFactor,
                index: getMovieVectorSearchIndexSpec().name,
                limit: limit,
            }
        },
        // The $group and $unwind is used to create a rank.
        {$group: {_id: null, docs: {$push: "$$ROOT"}}},
        {$unwind: {path: "$docs", includeArrayIndex: "vs_rank"}},
        {
            $addFields: {
                // RRF: 1 divided by rank + vector search rank constant.
                vs_score: {$divide: [1.0, {$add: ["$vs_rank", kRankConstant]}]}
            }
        },
        {
            $unionWith: {
                coll: collName,
                pipeline: [
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
                    }
                ]
            }
        },
        {
            $group: {
                _id: "$docs._id",
                docs: {$first: "$docs"},
                vs_score: {$max: {$ifNull: ["$vs_score", 0]}},
                fts_score: {$max: {$ifNull: ["$fts_score", 0]}}
            }
        },
        {$addFields: {score: {$add: ["$fts_score", "$vs_score"]}}},
        {$sort: {score: -1, _id: 1}},
        {$limit: limit},
        {$replaceRoot: {newRoot: "$docs"}}
    ];
    let results = coll.aggregate(hybridSearchQuery).toArray();

    assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.MOVIES), results);
}

const expectedResultIdOrder = [6, 4, 1, 5, 2, 3, 8, 9, 10, 12, 13, 14, 11, 7, 15];
runTest(expectedResultIdOrder);

dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});
