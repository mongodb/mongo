/*
 * Tests hybrid search with the rank fusion using the $rankFusion stage.
 * @tags: [ featureFlagRankFusionBasic, requires_fcv_81 ]
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

let testQuery = [
    {
        $rankFusion: {
            input: {
                pipelines: {
                    vector: [{
                        $vectorSearch: {
                            // Get the embedding for 'Tarzan the Ape Man', which has _id = 6.
                            queryVector: getMoviePlotEmbeddingById(6),
                            path: "plot_embedding",
                            numCandidates: limit * vectorSearchOverrequestFactor,
                            index: getMovieVectorSearchIndexSpec().name,
                            limit: limit,
                        }
                    }],
                    search: [
                        {
                            $search: {
                                index: getMovieSearchIndexSpec().name,
                                text: {query: "ape", path: ["fullplot", "title"]},
                            }
                        },
                        {$limit: limit}
                    ]
                }
            }
        },
    },
    {$limit: limit}
];

let results = coll.aggregate(testQuery).toArray();

let expectedResultIds = [6, 4, 1, 5, 2, 3, 8, 9, 10, 12, 13, 14, 11, 7, 15];
assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.MOVIES), results);

dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});
