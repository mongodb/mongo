/*
 * Tests applying weights to $rankFusion and $scoreFusion. Uses the same input pipelines as
 * ranked_fusion_test.js, but with different weighting to produce different result ordering. Also
 * tests that proper error codes are thrown for specifying bad pipeline weights.
 * @tags: [ featureFlagRankFusionFull, featureFlagSearchHybridScoringFull, requires_fcv_81 ]
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

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));

createSearchIndex(coll, getMovieSearchIndexSpec());
createSearchIndex(coll, getMovieVectorSearchIndexSpec());

// Builds a $rankFusion/$scoreFusion query with custom weights object.
function buildQueryWithWeights(weights, stageName) {
    const limit = 20;
    // Multiplication factor of limit for numCandidates in $vectorSearch.
    const vectorSearchOverrequestFactor = 10;
    const pipelines = {
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
    };
    let query = [];
    if (stageName === "$rankFusion") {
        query = [
            {
                $rankFusion: {input: {pipelines: pipelines}, combination: {weights}},
            },
            {$limit: limit}
        ];
    } else {
        // Must be $scoreFusion.
        query = [
            {
                $scoreFusion:
                    {input: {pipelines: pipelines, normalization: "none"}, combination: {weights}},
            },
            {$limit: limit}
        ];
    }
    return query;
}

// Asserts documents resulting from $rankFusion/$scoreFusion query are in expected order.
function runTest(weights, expectedResultIds) {
    let rankFusionQuery = buildQueryWithWeights(weights, "$rankFusion");
    let rankFusionResults = coll.aggregate(rankFusionQuery).toArray();
    assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.MOVIES),
                              rankFusionResults);
    let scoreFusionQuery = buildQueryWithWeights(weights, "$scoreFusion");
    let scoreFusionResults = coll.aggregate(scoreFusionQuery).toArray();
    assertDocArrExpectedFuzzy(buildExpectedResults(expectedResultIds, datasets.MOVIES),
                              scoreFusionResults);
}

// Asserts the $rankFusion/$scoreFusion query fails with the expected error code, given a bad
// weights object.
function runTestExpectError(weights, errorCode) {
    let rankFusionQuery = buildQueryWithWeights(weights, "$rankFusion");
    assert.throwsWithCode(function() {
        coll.aggregate(rankFusionQuery);
    }, errorCode);
    let scoreFusionQuery = buildQueryWithWeights(weights, "$scoreFusion");
    assert.throwsWithCode(function() {
        coll.aggregate(scoreFusionQuery);
    }, errorCode);
}

// Although you can't easily tell from the list of IDs, when cross-referencing with the data set, it
// is clear that the weights are applied properly if these result set orderings are returned.
runTest({vector: 1, search: 1}, [6, 4, 1, 5, 2, 3, 8, 9, 10, 12, 13, 14, 11, 7, 15]);
runTest({vector: 0, search: 1}, [6, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15]);
runTest({vector: 1, search: 0}, [6, 4, 8, 9, 10, 12, 13, 5, 1, 14, 3, 2, 11, 7, 15]);
runTest({vector: 5, search: 3}, [6, 4, 1, 5, 3, 2, 8, 9, 10, 12, 13, 14, 11, 7, 15]);
// Specifying a subset of weights is also valid.
runTest({vector: 1}, [6, 4, 1, 5, 2, 3, 8, 9, 10, 12, 13, 14, 11, 7, 15]);
runTest({search: 0}, [6, 4, 8, 9, 10, 12, 13, 5, 1, 14, 3, 2, 11, 7, 15]);
// No specified weights defaults to 1 for all pipelines.
runTest({}, [6, 4, 1, 5, 2, 3, 8, 9, 10, 12, 13, 14, 11, 7, 15]);

// Now test that improperly specified weights fail as expected.
// More weights than pipelines.
runTestExpectError({vector: 0.1, search: 0.2, a: 0.3}, 9460301);
// Single non-existent pipeline.
runTestExpectError({a: 0.1}, 9967500);
// One existent, and one non-existent pipeline.
runTestExpectError({vector: 0.1, a: 0.2}, 9967500);
// Non-numeric weight
runTestExpectError({vector: 0.1, search: "0.2"}, 13118);
// Negative weight
runTestExpectError({vector: 0.1, search: -0.2}, 9460300);

dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});
