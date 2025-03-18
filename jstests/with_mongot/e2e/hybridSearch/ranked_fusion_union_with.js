/*
 * Tests hybrid search with $rankFusion inside of a $unionWith subpipeline.
 * @tags: [ featureFlagRankFusionBasic, requires_fcv_81 ]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieSearchIndexSpec,
    getMovieVectorSearchIndexSpec
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {
    assertDocArrExpectedFuzzy,
    buildExpectedResults,
    datasets,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

// Create multiple collections in order to correctly run an identity $unionWith query.
const collAName = "search_rank_fusion_collA";
const collBName = "search_rank_fusion_collB";
const collA = db.getCollection(collAName);
const collB = db.getCollection(collBName);
collA.drop();
collB.drop();

assert.commandWorked(collA.insertMany(getMovieData()));

createSearchIndex(collA, getMovieSearchIndexSpec());
createSearchIndex(collA, getMovieVectorSearchIndexSpec());

const limit = 20;
// Multiplication factor of limit for numCandidates in $vectorSearch.
const vectorSearchOverrequestFactor = 10;

/*
 * Identity $unionWith.
 */
let collATestQuery = [
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

// Perform an identity $unionWith query. collB has no documents inside of it meaning that anything
// we "union" it with will just be the union-ed collection (in this case, srcColl).
let results =
    collB.aggregate([{$unionWith: {coll: collA.getName(), pipeline: collATestQuery}}]).toArray();

let expectedResultIds = [6, 4, 1, 5, 2, 3, 8, 9, 10, 12, 13, 14, 11, 7, 15];
assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.MOVIES), results);

/*
 * Two $rankFusion queries on the same dataset connected by a $unionWith.
 */
assert.commandWorked(collB.insertMany(getMovieData()));

let collBTestQuery = [
    {
        $rankFusion: {
            input: {
                pipelines: {
                    search: [
                        // $search for "romance" will only return the final two movies (14, 15).
                        {
                            $search: {
                                index: getMovieSearchIndexSpec().name,
                                text: {query: "romance", path: ["fullplot", "title"]},
                            }
                        },
                        {$limit: 5}
                    ]
                }
            }
        },
    },
    {$unionWith: {coll: collA.getName(), pipeline: collATestQuery}},
    {$limit: 10}
];

createSearchIndex(collB, getMovieSearchIndexSpec());
createSearchIndex(collB, getMovieVectorSearchIndexSpec());

results = collB.aggregate(collBTestQuery).toArray();

expectedResultIds = [14, 15, 6, 4, 1, 5, 2, 3, 8, 9];
assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.MOVIES), results);

dropSearchIndex(collA, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(collA, {name: getMovieVectorSearchIndexSpec().name});
dropSearchIndex(collB, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(collB, {name: getMovieVectorSearchIndexSpec().name});
