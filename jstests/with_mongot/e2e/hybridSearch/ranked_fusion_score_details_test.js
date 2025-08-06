/*
 * Tests hybrid search $rankFusion score details. This test focuses on ensuring that the structure
 * and contents of the produced scoreDetails field is correct.
 *
 * @tags: [ featureFlagRankFusionFull, requires_fcv_81 ]
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
    vectorStage,
} from "jstests/with_mongot/e2e_lib/hybrid_search_score_details_utils.js";

const coll = createMoviesCollWithSearchAndVectorIndex();

const stageType = "rank";

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
                input: {
                    pipelines: {vector: [vectorStage], search: [searchStageWithDetails, limitStage]}
                },
                combination: {weights: {search: 2}},
                scoreDetails: true,
            },
        },
        projectScoreAndScoreDetailsStage
    ];

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
        projectScoreAndScoreDetailsStage
    ];

    const results = coll.aggregate(testQuery).toArray();

    for (const foundDoc of results) {
        const [assertFieldPresent, subDetails, score] =
            checkOuterScoreDetails(stageType, foundDoc, 2);

        const secondVectorDetails = subDetails[0];
        const secondVectorSearchScore = checkVectorScoreDetails(
            stageType, assertFieldPresent, secondVectorDetails, "secondVector", 2.8);
        const vectorDetails = subDetails[1];
        const vectorSearchScore =
            checkVectorScoreDetails(stageType, assertFieldPresent, vectorDetails, "vector", 0.5);
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
                    pipelines: {vector: [vectorStage], search: [searchStageNoDetails, limitStage]}
                },
                scoreDetails: true,
            },
        },
        projectScoreAndScoreDetailsStage
    ];

    const results = coll.aggregate(testQuery).toArray();

    for (const foundDoc of results) {
        const [assertFieldPresent, subDetails, score] =
            checkOuterScoreDetails(stageType, foundDoc, 2);

        const searchDetails = subDetails[0];
        const searchScore = checkSearchScoreDetails(
            stageType, assertFieldPresent, searchDetails, "search", 1, false);
        const vectorDetails = subDetails[1];
        const vectorSearchScore =
            checkVectorScoreDetails(stageType, assertFieldPresent, vectorDetails, "vector", 1);
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
                input: {pipelines: {search: [searchStageWithDetails, limitStage]}},
                scoreDetails: true,
            },
        },
        projectScoreAndScoreDetailsStage
    ];

    const results = coll.aggregate(testQuery).toArray();

    for (const foundDoc of results) {
        const [assertFieldPresent, subDetails, score] =
            checkOuterScoreDetails(stageType, foundDoc, 1);

        const searchDetails = subDetails[0];
        const searchScore = checkSearchScoreDetails(
            stageType, assertFieldPresent, searchDetails, "search", 1, true);
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
                    pipelines: {vector: [vectorStage], search: [searchStageNoDetails, limitStage]}
                },
                scoreDetails: false,
            },
        },
        projectScoreStage
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
                input: {
                    pipelines: {vector: [vectorStage], search: [searchStageWithDetails, limitStage]}
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
 * Verify that when $rankFusion.scoreDetails is false and an input pipeline ($search) has
 * scoreDetails set to true, the aggregation succeeds when scoreDetails metadata is NOT projected
 * out.
 */
(function testQueryWithoutScoreDetailsMetadataProjectionWorksWhenRankFusionHasNoScoreDetails() {
    const testQuery = [
        {
            $rankFusion: {
                input: {pipelines: {search: [searchStageWithDetails, limitStage]}},
                combination: {weights: {search: 2}},
                scoreDetails: false,
            },
        },
        projectOutPlotEmbeddingStage
    ];

    assert.commandWorked(
        db.runCommand({aggregate: coll.getName(), pipeline: testQuery, cursor: {}}));
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
        projectOutPlotEmbeddingStage
    ];
    assert.commandWorked(
        db.runCommand({aggregate: coll.getName(), pipeline: testQuery, cursor: {}}));
    const results = coll.aggregate(testQuery).toArray();
    assert.eq(results, []);
})();

dropDefaultMovieSearchAndOrVectorIndexes();

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
    const geoNearCollName = jsTestName() + "_geoNear";
    const geoNearColl = db[geoNearCollName];

    assert.commandWorked(geoNearColl.insertMany(getRentalData()));

    // Index is blocking by default so that the query is only run after index has been made.
    createSearchIndex(geoNearColl, getRentalSearchIndexSpec());

    assert.commandWorked(geoNearColl.createIndex({"address.location.coordinates": "2d"}));

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
                            limitStage
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

        assert.eq(score, geoNearScore + searchScore);
    }
})();
