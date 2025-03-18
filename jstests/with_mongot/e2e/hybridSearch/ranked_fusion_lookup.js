/*
 * Tests hybrid search with $rankFusion inside of a $lookup subpipeline.
 * @tags: [ featureFlagRankFusionBasic, requires_fcv_81 ]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMovieSearchIndexSpec,
    getMovieVectorSearchIndexSpec
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {
    assertDocArrExpectedFuzzy,
    buildExpectedResults,
    datasets,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

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

/*
 * Identity $lookup.
 */

// Note: There is no $vectorSearch stage here as $vectorSearch is not allowed to be executed inside
// of a $lookup stage.
// TODO SERVER-88602: Add a $vectorSearch stage inside of the $rankFusion below. Copying the
// equivalent portion of ranked_fusion_union_with.js should be all that is needed.
let collATestQuery = [
    {
        $rankFusion: {
            input: {
                pipelines: {
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

// Perform an identity $lookup query.
let results = collA
                  .aggregate([
                      {$limit: 1},
                      {$lookup: {from: collA.getName(), pipeline: collATestQuery, as: "out"}},
                      {$unwind: "$out"},
                      {$replaceRoot: {newRoot: "$out"}}
                  ])
                  .toArray();

let expectedResultIds = [6, 1, 2, 3, 4, 5];
assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.MOVIES), results);

/*
 * Two $rankFusion queries on the same dataset connected by a $lookup.
 */
assert.commandWorked(collB.insertMany(getMovieData()));

// TODO SERVER-88602: Add a $vectorSearch stage inside of the $rankFusion below. Copying the
// equivalent portion of ranked_fusion_union_with.js should be all that is needed.
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
    {$lookup: {from: collA.getName(), pipeline: collATestQuery, as: "out"}},
    {$limit: 10}
];

createSearchIndex(collB, getMovieSearchIndexSpec());
createSearchIndex(collB, getMovieVectorSearchIndexSpec());

results = collB.aggregate(collBTestQuery).toArray();

/*
 * Because of how $lookup works, the initial output of the above query will be:
 *
 * [
 *     {
 *         id: 0,
 *         ...
 *         out: [{_id: 10, ...}, {_id: 11, ...}, ...] // Nested documents from $lookup
 *     },
 *     {
 *         id: 1,
 *         ...
 *         out: [{_id: 10, ...}, {_id: 11, ...}, ...] // Identical nested documents from $lookup
 *     },
 *     .
 *     .
 *     .
 * ]
 *
 * This means that in order to run assertDocArrExpectedFuzzy() as recommended for $rankedFusion
 * tests, it's necessary to do some post-parsing of the results to ensure that:
 *      1. The "top-level" documents (from $rankFusion being executed on collB) are separated from
 * the "nested" documents (from $lookup being executed on collA).
 *      2. The "nested" documents are appended after the "top-level" documents, without repeats.
 */
let collBResults = [];

// Keep track of seen ids to prevent repeated results.
let collAIds = new Set();
let collAResults = [];

results.forEach(doc => {
    // Get every part of the document except for the "out" array created by collA's lookup.
    let topLevelDoc = Object.fromEntries(Object.entries(doc).filter(([key]) => key !== "out"));

    collBResults.push(topLevelDoc);

    // Process nested documents from collA's lookup.
    if (doc.out && Array.isArray(doc.out)) {
        doc.out.forEach(nestedDoc => {
            // Add nested document if not already seen.
            if (!collAIds.has(nestedDoc._id)) {
                collAIds.add(nestedDoc._id);
                collAResults.push(nestedDoc);
            }
        });
    }
});

// Combine results.
let combinedResults = [...collBResults, ...collAResults];

expectedResultIds = [14, 15, 6, 1, 2, 3, 4, 5];
assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.MOVIES),
                          combinedResults);

dropSearchIndex(collA, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(collA, {name: getMovieVectorSearchIndexSpec().name});
dropSearchIndex(collB, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(collB, {name: getMovieVectorSearchIndexSpec().name});
