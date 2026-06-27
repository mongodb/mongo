/*
 * Tests hybrid search with $scoreFusion inside of a $lookup subpipeline.
 * @tags: [ featureFlagSearchHybridScoringFull, requires_fcv_82 ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";
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

const collAName = "search_score_fusion_collA";
const collBName = "search_score_fusion_collB";
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

const extensionsInHybridSearchEnabled = FeatureFlagUtil.isEnabled(
    db.getMongo(),
    "ExtensionsInsideHybridSearch",
);

/*
 * Identity $lookup.
 */

let collATestQuery = [
    {
        $scoreFusion: {
            input: {
                pipelines: {
                    vector: [
                        {
                            $vectorSearch: {
                                // Get the embedding for 'Tarzan the Ape Man', which has _id = 6.
                                queryVector: getMoviePlotEmbeddingById(6),
                                path: "plot_embedding",
                                numCandidates: limit * vectorSearchOverrequestFactor,
                                index: getMovieVectorSearchIndexSpec().name,
                                limit: limit,
                            },
                        },
                    ],
                    search: [
                        {
                            $search: {
                                index: getMovieSearchIndexSpec().name,
                                text: {query: "ape", path: ["fullplot", "title"]},
                            },
                        },
                        {$limit: limit},
                    ],
                },
                normalization: "none",
            },
            combination: {method: "avg"},
        },
    },
    {$limit: limit},
];

// Perform an identity $lookup query.
const identityLookupPipeline = [
    {$limit: 1},
    {$lookup: {from: collA.getName(), pipeline: collATestQuery, as: "out"}},
    {$unwind: "$out"},
    {$replaceRoot: {newRoot: "$out"}},
];
// TODO SERVER-121094 Remove this check when the feature flag is removed.
// $unionWith is only allowed inside of $lookup when featureFlagExtensionsInsideHybridSearch is enabled.
if (extensionsInHybridSearchEnabled) {
    let results = collA.aggregate(identityLookupPipeline).toArray();
    let expectedResultIds = [6, 4, 1, 5, 2, 3, 8, 9, 10, 12, 13, 14, 11, 7, 15];
    assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.MOVIES), results);
} else {
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collA.getName(), pipeline: identityLookupPipeline, cursor: {}}),
        51047,
    );
}

/*
 * Two $scoreFusion queries on the same dataset connected by a $lookup.
 */
assert.commandWorked(collB.insertMany(getMovieData()));

let collBTestQuery = [
    {
        $scoreFusion: {
            input: {
                pipelines: {
                    search: [
                        // $search for "romance" will only return the final two movies (14, 15).
                        {
                            $search: {
                                index: getMovieSearchIndexSpec().name,
                                text: {query: "romance", path: ["fullplot", "title"]},
                            },
                        },
                        {$limit: 5},
                    ],
                },
                normalization: "none",
            },
            combination: {method: "avg"},
        },
    },
    {$lookup: {from: collA.getName(), pipeline: collATestQuery, as: "out"}},
    {$limit: 10},
];

createSearchIndex(collB, getMovieSearchIndexSpec());
createSearchIndex(collB, getMovieVectorSearchIndexSpec());

// TODO SERVER-121094 Remove this check when the feature flag is removed.
// $unionWith is only allowed inside of $lookup when featureFlagExtensionsInsideHybridSearch is enabled.
if (extensionsInHybridSearchEnabled) {
    let results = collB.aggregate(collBTestQuery).toArray();

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
     * This means that in order to run assertDocArrExpectedFuzzy() as recommended for $scoreFusion
     * tests, it's necessary to do some post-parsing of the results to ensure that:
     *      1. The "top-level" documents (from $scoreFusion being executed on collB) are separated
     * from the "nested" documents (from $lookup being executed on collA).
     *      2. The "nested" documents are appended after the "top-level" documents, without repeats.
     */
    let collBResults = [];

    // Keep track of seen ids to prevent repeated results across both collB and collA results.
    let seenIds = new Set();
    let collAResults = [];

    // First pass: collect all top-level collB documents and register their IDs before processing
    // any nested lookups. This prevents a later collB document's ID from appearing in the nested
    // results of an earlier collB document.
    results.forEach((doc) => {
        let topLevelDoc = Object.fromEntries(Object.entries(doc).filter(([key]) => key !== "out"));
        collBResults.push(topLevelDoc);
        seenIds.add(topLevelDoc._id);
    });

    // Second pass: collect nested collA documents, deduplicating against all collB IDs.
    results.forEach((doc) => {
        if (doc.out && Array.isArray(doc.out)) {
            doc.out.forEach((nestedDoc) => {
                // Add nested document if not already seen in either collB or collA results.
                if (!seenIds.has(nestedDoc._id)) {
                    seenIds.add(nestedDoc._id);
                    collAResults.push(nestedDoc);
                }
            });
        }
    });

    // Combine results.
    let combinedResults = [...collBResults, ...collAResults];

    // collBResults = [14, 15], collAResults = remaining 13 movies from collA (14 and 15 excluded
    // because they are already in collBResults). Combined has 15 unique entries.
    assertDocArrExpectedFuzzy(
        buildExpectedResults([14, 15, 6, 4, 1, 5, 2, 3, 8, 9, 10, 12, 13, 11, 7], datasets.MOVIES),
        combinedResults,
    );
} else {
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collB.getName(), pipeline: collBTestQuery, cursor: {}}),
        51047,
    );
}

dropSearchIndex(collA, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(collA, {name: getMovieVectorSearchIndexSpec().name});
dropSearchIndex(collB, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(collB, {name: getMovieVectorSearchIndexSpec().name});
