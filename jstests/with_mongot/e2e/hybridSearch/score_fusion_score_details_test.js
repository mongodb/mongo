/*
 * Tests hybrid search $scoreFusion score details. This test focuses on ensuring that the structure
 * and contents of the produced scoreDetails field is correct.
 *
 * @tags: [ featureFlagSearchHybridScoringFull, requires_fcv_82 ]
 */

import {assertErrCodeAndErrMsgContains} from "jstests/aggregation/extras/utils.js";
import {createSearchIndex} from "jstests/libs/search.js";
import {
    createMoviesCollWithSearchAndVectorIndex,
    dropDefaultMovieSearchAndOrVectorIndexes,
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {getRentalData, getRentalSearchIndexSpec} from "jstests/with_mongot/e2e_lib/data/rentals.js";
import {
    checkGeoNearScoreDetails,
    checkOuterScoreDetails,
    checkSearchScoreDetails,
    checkVectorScoreDetails,
    fieldPresent,
    limitStage,
    projectOutPlotEmbeddingStage,
    projectScoreAndScoreDetailsStage,
    projectScoreStage,
    searchStageNoDetails,
    searchStageWithDetails,
    sortAscendingStage,
    vectorStage,
} from "jstests/with_mongot/e2e_lib/hybrid_search_score_details_utils.js";

const coll = createMoviesCollWithSearchAndVectorIndex();

const stageType = "score";

const testQueryGivenScoreDetails = (scoreDetails, pipelines, combination) => {
    let project = projectScoreAndScoreDetailsStage;
    if (!scoreDetails) {
        project = projectScoreStage;
    }
    let query = [
        {
            $scoreFusion: {
                input: {pipelines: pipelines, normalization: "none"},
                combination: combination,
                scoreDetails: scoreDetails,
            },
        },
        project,
        sortAscendingStage
    ];
    return query;
};

/**
 * Example of the expected score and scoreDetails metadata structure for a given results document:
 * "score" : 2.0521023273468018,
 * "details" : {
        "value" : 2.0521023273468018,
        "description" : "the value calculated by...",
        "normalization" : "none",
        "combination": {
            "method" : "average"
        },
        "details" : [
            {
                "inputPipelineName" : "search",
                "inputPipelineRawScore" : 1.5521023273468018,
                "weight" : 2,
                "value" : 3.1042046546936035,
                "details" : [ // search's score details
                    {
                        "value" : 1.5521023273468018,
                        "description" : "average of:",
                        "details" : [ {...} ]
                    }
                ]
            },
            {
                "inputPipelineName" : "vector",
                "inputPipelineRawScore" : 1,
                "weight" : 1,
                "value" : 1,
                "details" : [ ]
            }

        ]
    }
 */
(function testSearchScoreDetailsWithScoreFusionScoreDetailsTwoInputPipelines() {
    const testQuery = testQueryGivenScoreDetails(
        true,
        {vector: [vectorStage], search: [searchStageWithDetails, limitStage]},
        {weights: {vector: 1, search: 2}});

    const results = coll.aggregate(testQuery).toArray();

    for (const foundDoc of results) {
        const [assertFieldPresent, subDetails, score] =
            checkOuterScoreDetails(stageType, foundDoc, 2);

        const searchDetails = subDetails[0];
        const searchScore = checkSearchScoreDetails(
            stageType, assertFieldPresent, searchDetails, "search", 2, true);
        const vectorDetails = subDetails[1];
        const vectorSearchScore =
            checkVectorScoreDetails(stageType, assertFieldPresent, vectorDetails, "vector", 1);
        assert.eq(score, (searchScore + vectorSearchScore) / 2);
    }
})();

/**
 * Example of the expected score and scoreDetails metadata structure for a given results document:
 * "score" : 3.3,
 * "details" : {
        "value" : 3.3,
        "description" : "the value calculated by...",
        "normalization" : "none",
        "combination" : {
            "method" : "average"
        },
        "details" : [
            {
                "inputPipelineName" : "secondVector",
                "inputPipelineRawScore" : 1,
                "weight" : 2.8,
                "value" : 2.8,
                "details" : [ ]
            },
            {
                "inputPipelineName" : "vector",
                "inputPipelineRawScore" : 1,
                "weight" : 0.5,
                "value" : 0.5,
                "details" : [ ]
            }
        ]
    }
 */
(function testVectorSearchScoreDetailsWithScoreFusionScoreDetailsTwoInputPipelines() {
    const testQuery =
        testQueryGivenScoreDetails(true,
                                   {vector: [vectorStage], secondVector: [vectorStage]},
                                   {weights: {vector: 0.5, secondVector: 2.8}});

    const results = coll.aggregate(testQuery).toArray();

    for (const foundDoc of results) {
        const [assertFieldPresent, subDetails, score] =
            checkOuterScoreDetails(stageType, foundDoc, 2);

        const secondVectorDetails = subDetails[0];
        const secondVectorScore = checkVectorScoreDetails(
            stageType, assertFieldPresent, secondVectorDetails, "secondVector", 2.8);
        const vectorDetails = subDetails[1];
        const vectorSearchScore =
            checkVectorScoreDetails(stageType, assertFieldPresent, vectorDetails, "vector", 0.5);
        assert.eq(score, (secondVectorScore + vectorSearchScore) / 2);
    }
})();

/**
 * Example of the expected score and scoreDetails metadata structure for a given results document:
 * "score" : 2.5521023273468018,
 * "details" : {
        "value" : 2.5521023273468018,
        "description" : "the value calculated by...",
        "normalization" : "none",
        "combination" : {
            "method" : "average"
        },
        "details" : [
            {
                "inputPipelineName" : "search",
                "inputPipelineRawScore" : 1.5521023273468018,
                "weight" : 1,
                "value" : 1.5521023273468018,
                "details" : [ ]
            },
            {
                "inputPipelineName" : "vector",
                "inputPipelineRawScore" : 1,
                "weight" : 1,
                "value" : 1,
                "details" : [ ]
            }
        ]
    }
 */
(function testSearchScoreDetailsWithScoreFusionWithoutScoreDetailsNoCombinationTwoInputPipelines() {
    const testQuery = testQueryGivenScoreDetails(
        true, {vector: [vectorStage], search: [searchStageNoDetails, limitStage]}, {});

    const results = coll.aggregate(testQuery).toArray();

    for (const foundDoc of results) {
        const [assertFieldPresent, subDetails, score] =
            checkOuterScoreDetails(stageType, foundDoc, 2);

        const secondVectorDetails = subDetails[0];
        const secondVectorScore = checkSearchScoreDetails(
            stageType, assertFieldPresent, secondVectorDetails, "search", 1, false);
        const vectorDetails = subDetails[1];
        const vectorSearchScore =
            checkVectorScoreDetails(stageType, assertFieldPresent, vectorDetails, "vector", 1);
        assert.eq(score, (secondVectorScore + vectorSearchScore) / 2);
    }
})();

/**
 * Example of the expected score metadata structure for a given results document:
 * "score" : 2.5521023273468018
 */
(function testNoDetailsWithScoreFusionWithoutScoreDetailsTwoInputPipelines() {
    const testQuery = testQueryGivenScoreDetails(
        false, {vector: [vectorStage], search: [searchStageNoDetails, limitStage]}, {});

    const results = coll.aggregate(testQuery).toArray();

    for (const foundDoc of results) {
        // Assert that the score metadata has been set.
        assert(fieldPresent("score", foundDoc), foundDoc);
        const score = foundDoc["score"];
        assert.gte(score, 0);
    }
})();

/**
 * Verify that when $scoreFusion.scoreDetails is false and an input pipeline ($search) has
 * scoreDetails set to true, the aggregation fails when scoreDetails metadata is projected out.
 */
(function testScoreDetailsMetadataProjectionFailsWhenScoreFusionHasNoScoreDetails() {
    const testQuery = [
        {
            $scoreFusion: {
                input: {
                    pipelines:
                        {vector: [vectorStage], search: [searchStageWithDetails, limitStage]},
                    normalization: "none"
                },
                combination: {weights: {search: 2}},
                scoreDetails: false,
            },
        },
        projectScoreAndScoreDetailsStage
    ];

    assertErrCodeAndErrMsgContains(coll, testQuery, 40218, "query requires scoreDetails metadata");
})();

/**
 * Verify that when $scoreFusion.scoreDetails is false and an input pipeline ($search) has
 * scoreDetails set to true, the aggregation succeeds when scoreDetails metadata is NOT projected
 * out.
 */
(function testQueryWithoutScoreDetailsMetadataProjectionWorksWhenScoreFusionHasNoScoreDetails() {
    const testQuery = [
        {
            $scoreFusion: {
                input: {
                    pipelines: {search: [searchStageWithDetails, limitStage]},
                    normalization: "none"
                },
                combination: {weights: {search: 2}},
                scoreDetails: false,
            },
        },
        projectOutPlotEmbeddingStage
    ];

    assert.commandWorked(
        db.runCommand({aggregate: coll.getName(), pipeline: testQuery, cursor: {}}));
})();

dropDefaultMovieSearchAndOrVectorIndexes();

/**
 * Verify scoreDetails correctly projected when $scoreFusion takes a $geoNear input pipeline.
 *
    "_id" : 41,
    "score" : 0.04891591750396616,
    "details" : {
        "value" : 0.04891591750396616,
        "description" : "value output by reciprocal rank fusion algorithm...",
        "details" : [
            {
                "inputPipelineName" : "geoNear",
                "weight" : 2,
                "details" : [ ]
            },
            {
                "inputPipelineName" : "search",
                "weight" : 1,
                "value" : 2.7601585388183594,
                "details" : [ ]
            }
        ]
    }
}
 */
(function testQueryWithScoreDetailsForGeoNearInputPipeline() {
    const geoNearCollName = jsTestName() + "_geoNear";
    const geoNearColl = db[geoNearCollName];

    assert.commandWorked(geoNearColl.insertMany(getRentalData()));

    // Index is blocking by default so that the query is only run after index has been made.
    createSearchIndex(geoNearColl, getRentalSearchIndexSpec());

    assert.commandWorked(geoNearColl.createIndex({"address.location.coordinates": "2d"}));

    const testQuery = [
        {
            $scoreFusion: {
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
                            limitStage
                        ],
                        geoNear: [
                            {
                                $geoNear: {
                                    near: [-73.97713, 40.68675],
                                }
                            },
                            {$score: {score: {$meta: "geoNearDistance"}, normalization: "sigmoid"}},
                        ],
                    },
                    normalization: "none"
                },
                combination: {weights: {geoNear: 2}},
                scoreDetails: true,
            },
        },
        projectScoreAndScoreDetailsStage
    ];
    assert.commandWorked(
        db.runCommand({aggregate: geoNearCollName, pipeline: testQuery, cursor: {}}));
    const results = geoNearColl.aggregate(testQuery).toArray();
    for (const foundDoc of results) {
        const [assertFieldPresent, subDetails, score] =
            checkOuterScoreDetails(stageType, foundDoc, 2);

        // Check geoNear input pipeline details.
        const geoNearScore =
            checkGeoNearScoreDetails(stageType, assertFieldPresent, subDetails[0], "geoNear", 2);

        // Check search input pipeline details.
        const searchScore = checkSearchScoreDetails(
            stageType, assertFieldPresent, subDetails[1], "search", 1, false);

        assert.eq(score, (geoNearScore + searchScore) / 2);
    }
})();
