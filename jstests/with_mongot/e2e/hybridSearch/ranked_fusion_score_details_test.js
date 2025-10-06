/*
 * Tests hybrid search $rankFusion score details. This test focuses on ensuring that the structure
 * and contents of the produced scoreDetails field is correct.
 *
 * @tags: [ featureFlagRankFusionFull, requires_fcv_81 ]
 */

import {assertErrCodeAndErrMsgContains} from "jstests/aggregation/extras/utils.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieSearchIndexSpec,
    getMovieVectorSearchIndexSpec
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {getRentalData, getRentalSearchIndexSpec} from "jstests/with_mongot/e2e_lib/data/rentals.js";

const collName = jsTestName();
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

const vectorStageSpec = {
    // Get the embedding for 'Tarzan the Ape Man', which has _id = 6.
    queryVector: getMoviePlotEmbeddingById(6),
    path: "plot_embedding",
    numCandidates: limit * vectorSearchOverrequestFactor,
    index: getMovieVectorSearchIndexSpec().name,
    limit: limit,
};
const vectorStage = {
    $vectorSearch: vectorStageSpec
};

const searchStageSpec = {
    index: getMovieSearchIndexSpec().name,
    text: {query: "ape", path: ["fullplot", "title"]},
    scoreDetails: true
};

const searchStage = {
    $search: searchStageSpec
};

const searchStageSpecNoDetails = {
    index: getMovieSearchIndexSpec().name,
    text: {query: "ape", path: ["fullplot", "title"]},
    scoreDetails: false
};

const searchStageNoDetails = {
    $search: searchStageSpecNoDetails
};

const calculateReciprocalRankFusionScore = (weight, rank) => {
    return (weight * (1 / (60 + rank)));
};

const scoreDetailsDescription =
    "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / (60 " +
    "+ rank))) across input pipelines from which this document is output, from:";

/**
 * All input pipelines should contain the following fields when $rankFusion's scoreDetails is
 * enabled: inputPipelineName, rank, and weight. Only inputPipelineName's and weight's values are
 * constant across the results.
 */
function checkDefaultPipelineScoreDetails(assertFieldPresent, subDetails, pipelineName, weight) {
    assertFieldPresent("inputPipelineName", subDetails);
    assert.eq(subDetails["inputPipelineName"], pipelineName);
    assertFieldPresent("rank", subDetails);
    assertFieldPresent("weight", subDetails);
    assert.eq(subDetails["weight"], weight);
}

/**
 * Checks the scoreDetails (inputPipelineName, rank, weight) for a search input pipeline. If a
 * document was ouput from the input pipeline (value field is present in scoreDetails), then check
 * that the value and details fields are present. If the search input pipeline has scoreDetails
 * enabled, check the description field is accurate and that the pipeline's scoreDetails aren't
 * empty. Returns the RRF score for this input pipeline.
 */
function checkSearchScoreDetails(
    assertFieldPresent, subDetails, pipelineName, weight, isScoreDetails) {
    assertFieldPresent("inputPipelineName", subDetails);
    assert.eq(subDetails["inputPipelineName"], pipelineName);
    assertFieldPresent("rank", subDetails);
    // If there isn't a value, we didn't get this back from search at all.
    let searchScore = 0;
    if (subDetails.hasOwnProperty("value")) {
        assertFieldPresent("weight", subDetails);
        assert.eq(subDetails["weight"], weight);
        assertFieldPresent("value", subDetails);  // Output of rank calculation.
        assertFieldPresent("details", subDetails);
        if (isScoreDetails) {
            assertFieldPresent("description", subDetails);
            assert.eq(subDetails["description"], "sum of:");
            // Note we won't check the shape of the search scoreDetails beyond here.
            assert.neq(subDetails["details"], []);
        } else {
            assert.eq(subDetails["details"], []);
        }
        searchScore = calculateReciprocalRankFusionScore(subDetails["weight"], subDetails["rank"]);
    } else {
        assert.eq(subDetails["rank"], "NA");
    }
    return searchScore;
}

/**
 * Checks the scoreDetails (inputPipelineName, rank, weight, details) for a vectorSearch input
 * pipeline. Note that vectorSearch input pipeline do not have scoreDetails so the details field
 * should always be an empty array. Returns the RRF score for this input pipeline.
 */
function checkVectorScoreDetails(assertFieldPresent, subDetails, pipelineName, weight) {
    checkDefaultPipelineScoreDetails(assertFieldPresent, subDetails, pipelineName, weight);
    assertFieldPresent("value", subDetails);  // Original 'score' AKA vectorSearchScore.
    assertFieldPresent("details", subDetails);
    assert.eq(subDetails["details"], []);
    const vectorSearchScore =
        calculateReciprocalRankFusionScore(subDetails["weight"], subDetails["rank"]);
    return vectorSearchScore;
}

/**
 * Checks the scoreDetails (inputPipelineName, rank, weight, details) for a geoNear input
 * pipeline. Note that geoNear input pipeline do not have scoreDetails so the details field
 * should always be an empty array. Returns the RRF score for this input pipeline.
 */
function checkGeoNearScoreDetails(assertFieldPresent, subDetails, pipelineName, weight) {
    checkDefaultPipelineScoreDetails(assertFieldPresent, subDetails, pipelineName, weight);
    assertFieldPresent("details", subDetails);
    assert.eq(subDetails["details"], []);
    const geoNearScore =
        calculateReciprocalRankFusionScore(subDetails["weight"], subDetails["rank"]);
    return geoNearScore;
}

/**
 * For each document or result, check the follwoing fields in the outer scoreDetails: score,
 * details, value, description, and that the subDetails array contains two entries, 1 for each input
 * pipeline.
 */
function checkOuterScoreDetails(foundDoc, numInputPipelines) {
    // Assert that the score metadata has been set.
    assert(fieldPresent("score", foundDoc), foundDoc);
    const score = foundDoc["score"];
    assert(fieldPresent("details", foundDoc), foundDoc);
    const details = foundDoc["details"];
    assert(fieldPresent("value", details), details);
    // We don't care about the actual score, just assert that its been calculated.
    assert.gt(details["value"], 0, details);
    // Assert that the score metadata is the same value as what scoreDetails set.
    assert.eq(details["value"], score);
    assert(fieldPresent("description", details), details);
    assert.eq(details["description"], scoreDetailsDescription);

    function assertFieldPresent(field, obj) {
        assert(fieldPresent(field, obj),
               `Looked for ${field} in ${tojson(obj)}. Full details: ${tojson(details)}`);
    }
    // Description of rank fusion. Wrapper on both search / vector.
    assertFieldPresent("details", details);
    const subDetails = details["details"];
    assert.eq(subDetails.length, numInputPipelines);

    return [assertFieldPresent, subDetails, score];
}

function fieldPresent(field, containingObj) {
    return containingObj.hasOwnProperty(field);
}

/**
 * Test search/vectorSearch where only search has scoreDetails.
 * "score" : 0.04918032786885246,
 * "details" : {
        "value" : 0.04918032786885246,
        "description" : "value output by reciprocal rank fusion algorithm...",
        "details" : [
            {
                "inputPipelineName" : "search",
                "rank" : 1,
                "weight" : 2,
                "value" : 1.5521023273468018,
                "description" : "sum of:",
                "details" : [
                    // $search scoreDetails go here
                ]
            },
            {
                "inputPipelineName" : "vector",
                "rank" : 1,
                "weight" : 1,
                "value" : 1,
                "details" : [ ]
            }
        ]
    }
 */
(function testSearchScoreDetailsWithRankFusionScoreDetailsTwoInputPipelines() {
    const testQuery = [
        {
            $rankFusion: {
                input: {pipelines: {vector: [vectorStage], search: [searchStage, {$limit: limit}]}},
                combination: {weights: {search: 2}},
                scoreDetails: true,
            },
        },
        {$project: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}}
    ];

    const results = coll.aggregate(testQuery).toArray();

    for (const foundDoc of results) {
        const [assertFieldPresent, subDetails, score] = checkOuterScoreDetails(foundDoc, 2);

        const searchDetails = subDetails[0];
        const searchScore =
            checkSearchScoreDetails(assertFieldPresent, searchDetails, "search", 2, true);
        const vectorDetails = subDetails[1];
        const vectorSearchScore =
            checkVectorScoreDetails(assertFieldPresent, vectorDetails, "vector", 1);
        assert.eq(score, searchScore + vectorSearchScore);
    }
})();

/**
 * Test vectorSearch/vectorSearch where neither has score details.
 * "score" : 0.054098360655737705,
 * "details" : {
        "value" : 0.054098360655737705,
        "description" : "value output by reciprocal rank fusion algorithm...",
        "details" : [
            {
                "inputPipelineName" : "secondVector",
                "rank" : 1,
                "weight" : 2.8,
                "value" : 1,
                "details" : [ ]
            },
            {
                "inputPipelineName" : "vector",
                "rank" : 1,
                "weight" : 0.5,
                "value" : 1,
                "details" : [ ]
            }
        ]
    }
 */
(function testVectorSearchWithRankFusionScoreDetailsTwoInputPipelines() {
    const testQuery = [
        {
            $rankFusion: {
                input: {pipelines: {vector: [vectorStage], secondVector: [vectorStage]}},
                combination: {weights: {vector: 0.5, secondVector: 2.8}},
                scoreDetails: true,
            },
        },
        {$project: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}}
    ];

    const results = coll.aggregate(testQuery).toArray();

    for (const foundDoc of results) {
        const [assertFieldPresent, subDetails, score] = checkOuterScoreDetails(foundDoc, 2);

        const secondVectorDetails = subDetails[0];
        const secondVectorSearchScore =
            checkVectorScoreDetails(assertFieldPresent, secondVectorDetails, "secondVector", 2.8);
        const vectorDetails = subDetails[1];
        const vectorSearchScore =
            checkVectorScoreDetails(assertFieldPresent, vectorDetails, "vector", 0.5);
        assert.eq(score, secondVectorSearchScore + vectorSearchScore);
    }
})();

/**
 * Test search/vectorSearch where search scoreDetails is off but $rankFusion's scoreDetails is on.
 * "score" : 0.03278688524590164,
 * "details" : {
        "value" : 0.03278688524590164,
        "description" : "value output by reciprocal rank fusion algorithm...",
        "details" : [
            {
                "inputPipelineName" : "search",
                "rank" : 1,
                "weight" : 1,
                "value" : 1.5521023273468018,
                "details" : [ ]
            },
            {
                "inputPipelineName" : "vector",
                "rank" : 1,
                "weight" : 1,
                "value" : 1,
                "details" : [ ]
            }
        ]
    }
 */
(function testVectorSearchAndSearchNoScoreDetailsWithRankFusionScoreDetailsTwoInputPipelines() {
    const testQuery = [
        {
            $rankFusion: {
                input: {
                    pipelines:
                        {vector: [vectorStage], search: [searchStageNoDetails, {$limit: limit}]}
                },
                scoreDetails: true,
            },
        },
        {$project: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}}
    ];

    const results = coll.aggregate(testQuery).toArray();

    for (const foundDoc of results) {
        const [assertFieldPresent, subDetails, score] = checkOuterScoreDetails(foundDoc, 2);

        const searchDetails = subDetails[0];
        const searchScore =
            checkSearchScoreDetails(assertFieldPresent, searchDetails, "search", 1, false);
        const vectorDetails = subDetails[1];
        const vectorSearchScore =
            checkVectorScoreDetails(assertFieldPresent, vectorDetails, "vector", 1);
        assert.eq(score, searchScore + vectorSearchScore);
    }
})();

/**
 * Test $rankFusion with scoreDetails with 1 search input pipeline that has scoreDetails.
 * "score" : 0.01639344262295082,
 * "details" : {
        "value" : 0.01639344262295082,
        "description" : "value output by reciprocal rank fusion algorithm...",
        "details" : [
            {
                "inputPipelineName" : "search",
                "rank" : 1,
                "weight" : 1,
                "value" : 1.5521023273468018,
                "description" : "sum of:",
                "details" : [
                    {...} // search's scoreDetails
                ]
            }
        ]
    }
 */
(function testSearchWithScoreDetailsWithRankFusionScoreDetailsOneInputPipeline() {
    const testQuery = [
        {
            $rankFusion: {
                input: {pipelines: {search: [searchStage, {$limit: limit}]}},
                scoreDetails: true,
            },
        },
        {$project: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}}
    ];

    const results = coll.aggregate(testQuery).toArray();

    for (const foundDoc of results) {
        const [assertFieldPresent, subDetails, score] = checkOuterScoreDetails(foundDoc, 1);

        const searchDetails = subDetails[0];
        const searchScore =
            checkSearchScoreDetails(assertFieldPresent, searchDetails, "search", 1, true);
        assert.eq(score, searchScore);
    }
})();

/**
 * Test search/vectorSearch where search scoreDetails is off and $rankFusion's scoreDetails is off.
 * { "_id" : 6, "score" : 0.03278688524590164 }
 */
(function testVectorSearchAndSearchNoScoreDetailsWithRankFusionNoScoreDetailsTwoInputPipelines() {
    const testQuery = [
        {
            $rankFusion: {
                input: {
                    pipelines:
                        {vector: [vectorStage], search: [searchStageNoDetails, {$limit: limit}]}
                },
                scoreDetails: false,
            },
        },
        {$project: {score: {$meta: "score"}}}
    ];

    const results = coll.aggregate(testQuery).toArray();

    for (const foundDoc of results) {
        // Assert that the score metadata has been set.
        assert(fieldPresent("score", foundDoc), foundDoc);
        const score = foundDoc["score"];
        assert.gte(score, 0);
    }
})();

/**
 * Verify that when $rankFusion.scoreDetails is false and an input pipeline ($search) has
 * scoreDetails set to true, the aggregation fails when scoreDetails metadata is projected out.
 */
(function testScoreDetailsMetadataProjectionFailsWhenRankFusionHasNoScoreDetails() {
    const testQuery = [
        {
            $rankFusion: {
                input: {pipelines: {vector: [vectorStage], search: [searchStage, {$limit: limit}]}},
                combination: {weights: {search: 2}},
                scoreDetails: false,
            },
        },
        {$project: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}}
    ];

    assertErrCodeAndErrMsgContains(coll, testQuery, 40218, "query requires scoreDetails metadata");
})();

/**
 * Verify that when $rankFusion.scoreDetails is false and an input pipeline ($search) has
 * scoreDetails set to true, the aggregation succeeds when scoreDetails metadata is NOT projected
 * out.
 */
(function testQueryWithoutScoreDetailsMetadataProjectionWorksWhenRankFusionHasNoScoreDetails() {
    const testQuery = [
        {
            $rankFusion: {
                input: {pipelines: {search: [searchStage, {$limit: limit}]}},
                combination: {weights: {search: 2}},
                scoreDetails: false,
            },
        },
        {$project: {plot_embedding: 0}}
    ];

    assert.commandWorked(db.runCommand({aggregate: collName, pipeline: testQuery, cursor: {}}));
})();

/**
 * Verify that when $rankFusion.scoreDetails is true and an input pipeline doesn't set
 * score/scoreDetails metadata, the projected scoreDetails is empty.
 */
(function testQueryWithScoreDetailsForNoScoreOrScoreDetailsGeneratingPipeline() {
    const testQuery = [
        {
            $rankFusion: {
                input: {pipelines: {matchAndSort: [{$match: {title: "ape"}}, {$sort: {title: 1}}]}},
                combination: {weights: {matchAndSort: 2}},
                scoreDetails: true,
            },
        },
        {$project: {plot_embedding: 0}}
    ];
    assert.commandWorked(db.runCommand({aggregate: collName, pipeline: testQuery, cursor: {}}));
    const results = coll.aggregate(testQuery).toArray();
    assert.eq(results, []);
})();

// TODO SERVER-93218 Test scoreDetails with nested rankFusion.
dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});

/**
 * Verify scoreDetails correctly projected when $rankFusion takes a $geoNear input pipeline.
 *
    "_id" : 41,
    "score" : 0.04891591750396616,
    "details" : {
        "value" : 0.04891591750396616,
        "description" : "value output by reciprocal rank fusion algorithm...",
        "details" : [
            {
                "inputPipelineName" : "geoNear",
                "rank" : 1,
                "weight" : 2,
                "details" : [ ]
            },
            {
                "inputPipelineName" : "search",
                "rank" : 2,
                "weight" : 1,
                "value" : 2.7601585388183594,
                "details" : [ ]
            }
        ]
    }
}
 */
(function testQueryWithScoreDetailsForGeoNearInputPipeline() {
    coll.drop();

    assert.commandWorked(coll.insertMany(getRentalData()));

    // Index is blocking by default so that the query is only run after index has been made.
    createSearchIndex(coll, getRentalSearchIndexSpec());

    assert.commandWorked(coll.createIndex({"address.location.coordinates": "2d"}));

    const testQuery = [
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        search: [
                            {
                                $search: {
                                    index: getRentalSearchIndexSpec().name,
                                    text: {
                                        query: "brooklyn",
                                        path: [
                                            "name",
                                            "summary",
                                            "description",
                                            "neighborhood_overview",
                                        ],
                                    },
                                }
                            },
                            {$limit: limit}
                        ],
                        geoNear: [{
                            $geoNear: {
                                near: [-73.97713, 40.68675],
                            }
                        }],
                    }
                },
                combination: {weights: {geoNear: 2}},
                scoreDetails: true,
            },
        },
        {$project: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}}
    ];
    assert.commandWorked(db.runCommand({aggregate: collName, pipeline: testQuery, cursor: {}}));
    const results = coll.aggregate(testQuery).toArray();
    for (const foundDoc of results) {
        const [assertFieldPresent, subDetails, score] = checkOuterScoreDetails(foundDoc, 2);

        // Check geoNear input pipeline details.
        const geoNearScore =
            checkGeoNearScoreDetails(assertFieldPresent, subDetails[0], "geoNear", 2);

        // Check search input pipeline details.
        const searchScore =
            checkSearchScoreDetails(assertFieldPresent, subDetails[1], "search", 1, false);

        assert.eq(score, geoNearScore + searchScore);
    }

    dropSearchIndex(coll, {name: getRentalSearchIndexSpec().name});
})();
