/*
 * Tests hybrid search $scoreFusion score details. This test focuses on ensuring that the structure
 * and contents of the produced scoreDetails field is correct.
 *
 * @tags: [ featureFlagSearchHybridScoringFull, requires_fcv_81 ]
 */

import {assertErrCodeAndErrMsgContains} from "jstests/aggregation/extras/utils.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieSearchIndexSpec,
    getMovieVectorSearchIndexSpec
} from "jstests/with_mongot/e2e_lib/data/movies.js";

const collName = "search_score_fusion";
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));
createSearchIndex(coll, getMovieSearchIndexSpec());

createSearchIndex(coll, getMovieVectorSearchIndexSpec());

const limit = 20;
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

const sortAscending = {
    $sort: {_id: 1}
};

const projectScoreScoreDetails = {
    $project: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}
};

const projectScore = {
    $project: {score: {$meta: "score"}}
};

const limitStage = {
    $limit: limit
};

const scoreFusionDetailsDescription =
    "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:";

const scoreDetailsDescription =
    "the score calculated from multiplying a weight in the range [0,1] with either a normalized or nonnormalized value:";

function fieldPresent(field, containingObj) {
    return containingObj.hasOwnProperty(field);
}

const testQueryGivenScoreDetails = (scoreDetails, pipelines, combination) => {
    let project = projectScoreScoreDetails;
    if (!scoreDetails) {
        project = projectScore;
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
        sortAscending
    ];
    return query;
};

// Test search/vectorSearch where only search has scoreDetails.
let combinations = {weights: {vector: 1, search: 2}};

let results =
    coll
        .aggregate(testQueryGivenScoreDetails(
            true, {vector: [vectorStage], search: [searchStage, limitStage]}, combinations))
        .toArray();

let resultsNoScoreDetails =
    coll
        .aggregate(testQueryGivenScoreDetails(
            false, {vector: [vectorStage], search: [searchStage, limitStage]}, combinations))
        .toArray();

// Run $scoreFusion's first pipeline input. We will use the score value it calculates to assert
// that the calculated rawScore for the first input pipeline is correct.
let inputPipeline1RawScoreExpectedResults =
    coll.aggregate([vectorStage, projectScore, sortAscending]).toArray();

// Run $scoreFusion's second pipeline input. We will use the score value it calculates to assert
// that the calculated rawScore for the second input pipeline is correct.
let inputPipeline2RawScoreExpectedResults =
    coll.aggregate([searchStage, limitStage, projectScore, sortAscending]).toArray();

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
for (let i = 0; i < results.length; i++) {
    const foundDoc = results[i];
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
    // Assert that the top-level value has the same value as the 'score' metadata which is set
    // when the same $scoreFusion pipeline is run without scoreDetails.
    assert.eq(details["value"], resultsNoScoreDetails[i]["score"]);
    assert(fieldPresent("description", details), details);
    assert.eq(details["description"], scoreFusionDetailsDescription);
    assert.eq(details["normalization"], "none");
    const combination = details["combination"];
    assert(fieldPresent("method", combination), combination);
    assert.eq(combination["method"], "average");

    function assertFieldPresent(field, obj) {
        assert(fieldPresent(field, obj),
               `Looked for ${field} in ${tojson(obj)}. Full details: ${tojson(details)}`);
    }
    // Description of score fusion. Wrapper on both search / vector.
    assertFieldPresent("details", details);
    const subDetails = details["details"];
    assert.eq(subDetails.length, 2);

    const searchDetails = subDetails[0];
    assertFieldPresent("inputPipelineName", searchDetails);
    assert.eq(searchDetails["inputPipelineName"], "search");
    assertFieldPresent("inputPipelineRawScore", searchDetails);
    if (i < inputPipeline2RawScoreExpectedResults.length) {
        assert.eq(searchDetails["inputPipelineRawScore"],
                  inputPipeline2RawScoreExpectedResults[i]["score"]);
    }
    assertFieldPresent("weight", searchDetails);
    assert.eq(searchDetails["weight"], 2);
    assertFieldPresent("value", searchDetails);
    // No normalization applied to score value in this query, so the total score is the raw score
    // multiplied by the weight.
    assert.eq(searchDetails["value"],
              searchDetails["weight"] * searchDetails["inputPipelineRawScore"]);
    const searchScoreValue = searchDetails["value"];
    // If there isn't a details, we didn't get this back from search at all.
    if (searchDetails.hasOwnProperty("details")) {
        assertFieldPresent("details", searchDetails);
        assert.neq(searchDetails["details"], []);
        const searchSearchDetails = searchDetails["details"];
        assertFieldPresent("value", searchSearchDetails);
        assert.eq(searchSearchDetails["value"], searchDetails["inputPipelineRawScore"]);
        assertFieldPresent("details",
                           searchSearchDetails);  // Not checking description contents, just that
                                                  // its present and not our placeholder value.
        assert.neq(searchSearchDetails["details"], []);
    }
    const vectorDetails = subDetails[1];
    assertFieldPresent("inputPipelineName", vectorDetails);
    assert.eq(vectorDetails["inputPipelineName"], "vector");
    assertFieldPresent("inputPipelineRawScore", vectorDetails);
    if (i < inputPipeline1RawScoreExpectedResults.length) {
        assert.eq(vectorDetails["inputPipelineRawScore"],
                  inputPipeline1RawScoreExpectedResults[i]["score"]);
    }
    assertFieldPresent("weight", vectorDetails);
    assert.eq(vectorDetails["weight"], 1);
    const vectorScoreValue = vectorDetails["value"];
    assertFieldPresent("details", vectorDetails);
    assert.eq(vectorDetails["details"], []);
    assert.eq(score, (vectorScoreValue + searchScoreValue) / 2);
}

// Test vectorSearch/vectorSearch where neither has score details.
combinations = {
    weights: {vector: 0.5, secondVector: 2.8}
};

results =
    coll.aggregate(testQueryGivenScoreDetails(
                       true, {vector: [vectorStage], secondVector: [vectorStage]}, combinations))
        .toArray();

resultsNoScoreDetails =
    coll.aggregate(testQueryGivenScoreDetails(
                       false, {vector: [vectorStage], secondVector: [vectorStage]}, combinations))
        .toArray();

// Run $scoreFusion's first pipeline input.
inputPipeline1RawScoreExpectedResults =
    coll.aggregate([vectorStage, projectScore, sortAscending]).toArray();

// Run $scoreFusion's second pipeline input.
inputPipeline2RawScoreExpectedResults =
    coll.aggregate([vectorStage, projectScore, sortAscending]).toArray();

/**
 * Example of the expected score and scoreDetails metadata structure for a given results
 document:
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
for (let i = 0; i < results.length; i++) {
    const foundDoc = results[i];
    // Assert that the score metadata has been set.
    assert(fieldPresent("score", foundDoc), foundDoc);
    const score = foundDoc["score"];
    assert(fieldPresent("details", foundDoc), foundDoc);
    const details = foundDoc["details"];
    assert(fieldPresent("value", details), details);
    // We don't care about the actual score, just assert that its been calculated.
    assert.gt(details["value"], 0);
    // Assert that the score metadata is the same value as what scoreDetails set.
    assert.eq(details["value"], score);
    // Assert that the top-level value has the same value as the 'score' metadata which is set
    // when the same $scoreFusion pipeline is run without scoreDetails.
    assert.eq(details["value"], resultsNoScoreDetails[i]["score"]);
    assert(fieldPresent("description", details), details);
    assert.eq(details["description"], scoreFusionDetailsDescription);
    assert.eq(details["normalization"], "none");
    const combination = details["combination"];
    assert(fieldPresent("method", combination), combination);
    assert.eq(combination["method"], "average");

    function assertFieldPresent(field, obj) {
        assert(fieldPresent(field, obj),
               `Looked for ${field} in ${tojson(obj)}. Full details: ${tojson(details)}`);
    }
    // Description of score fusion. Wrapper on both secondVector / vector.
    assertFieldPresent("details", details);
    const subDetails = details["details"];
    assert.eq(subDetails.length, 2);

    const secondVectorDetails = subDetails[0];
    assertFieldPresent("inputPipelineName", secondVectorDetails);
    assert.eq(secondVectorDetails["inputPipelineName"], "secondVector");
    assertFieldPresent("inputPipelineRawScore", secondVectorDetails);
    if (i < inputPipeline2RawScoreExpectedResults.length) {
        assert.eq(secondVectorDetails["inputPipelineRawScore"],
                  inputPipeline2RawScoreExpectedResults[i]["score"]);
    }
    const inputPipeline2RawScore = secondVectorDetails["inputPipelineRawScore"];
    assertFieldPresent("weight", secondVectorDetails);
    assert.eq(secondVectorDetails["weight"], 2.8);
    assertFieldPresent("value",
                       secondVectorDetails);  // Original 'score' AKA vectorSearchScore.
    assert.eq(secondVectorDetails["value"], inputPipeline2RawScore * 2.8);
    const secondVectorScoreValue = secondVectorDetails["value"];
    assertFieldPresent("details",
                       secondVectorDetails);  // Not checking description contents, just that
                                              // its present and not our placeholder value.
    assert.eq(secondVectorDetails["details"], []);

    const vectorDetails = subDetails[1];
    assertFieldPresent("inputPipelineName", vectorDetails);
    assert.eq(vectorDetails["inputPipelineName"], "vector");
    assertFieldPresent("inputPipelineRawScore", vectorDetails);
    if (i < inputPipeline1RawScoreExpectedResults.length) {
        assert.eq(vectorDetails["inputPipelineRawScore"],
                  inputPipeline1RawScoreExpectedResults[i]["score"]);
    }
    const inputPipeline1RawScore = vectorDetails["inputPipelineRawScore"];
    assertFieldPresent("weight", vectorDetails);
    assert.eq(vectorDetails["weight"], 0.5);
    assertFieldPresent("value", vectorDetails);  // Original 'score' AKA vectorSearchScore.
    assert.eq(vectorDetails["value"], inputPipeline1RawScore * 0.5);
    const firstVectorScoreValue = vectorDetails["value"];
    assertFieldPresent("details", vectorDetails);
    assert.eq(vectorDetails["details"], []);
    assert.eq(score, (firstVectorScoreValue + secondVectorScoreValue) / 2);
}

// Test search/vectorSearch where search scoreDetails is off but $scoreFusion's scoreDetails is on.
const searchStageSpecNoDetails = {
    index: getMovieSearchIndexSpec().name,
    text: {query: "ape", path: ["fullplot", "title"]},
    scoreDetails: false
};

const searchStageNoDetails = {
    $search: searchStageSpecNoDetails
};

results = coll
              .aggregate(testQueryGivenScoreDetails(
                  true, {vector: [vectorStage], search: [searchStageNoDetails, limitStage]}, {}))
              .toArray();

resultsNoScoreDetails =
    coll
        .aggregate(testQueryGivenScoreDetails(
            false, {vector: [vectorStage], search: [searchStageNoDetails, limitStage]}, {}))
        .toArray();

// Run $scoreFusion's first pipeline input.
inputPipeline1RawScoreExpectedResults =
    coll.aggregate([vectorStage, projectScore, sortAscending]).toArray();

// Run $scoreFusion's second pipeline input.
inputPipeline2RawScoreExpectedResults =
    coll.aggregate([searchStageNoDetails, limitStage, projectScore, sortAscending]).toArray();

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
for (let i = 0; i < results.length; i++) {
    const foundDoc = results[i];
    // Assert that the score metadata has been set.
    assert(fieldPresent("score", foundDoc), foundDoc);
    const score = foundDoc["score"];
    assert(fieldPresent("details", foundDoc), foundDoc);
    const details = foundDoc["details"];
    assert(fieldPresent("value", details), details);
    // We don't care about the actual score, just assert that its been calculated.
    assert.gt(details["value"], 0);
    // Assert that the score metadata is the same value as what scoreDetails set.
    assert.eq(details["value"], score);
    // Assert that the top-level value has the same value as the 'score' metadata which is set
    // when the same $scoreFusion pipeline is run without scoreDetails.
    assert.eq(details["value"], resultsNoScoreDetails[i]["score"]);
    assert(fieldPresent("description", details), details);
    assert.eq(details["description"], scoreFusionDetailsDescription);
    assert.eq(details["normalization"], "none");
    const combination = details["combination"];
    assert(fieldPresent("method", combination), combination);
    assert.eq(combination["method"], "average");

    function assertFieldPresent(field, obj) {
        assert(fieldPresent(field, obj),
               `Looked for ${field} in ${tojson(obj)}. Full details: ${tojson(details)}`);
    }
    // Description of score fusion. Wrapper on both search / vector.
    assertFieldPresent("details", details);
    const subDetails = details["details"];
    assert.eq(subDetails.length, 2);

    const searchDetails = subDetails[0];
    assertFieldPresent("inputPipelineName", searchDetails);
    assert.eq(searchDetails["inputPipelineName"], "search");
    assertFieldPresent("inputPipelineRawScore", searchDetails);
    if (i < inputPipeline2RawScoreExpectedResults.length) {
        assert.eq(searchDetails["inputPipelineRawScore"],
                  inputPipeline2RawScoreExpectedResults[i]["score"]);
    }
    assertFieldPresent("weight", searchDetails);
    assert.eq(searchDetails["weight"], 1);
    assertFieldPresent("value", searchDetails);
    assert.eq(searchDetails["value"],
              searchDetails["weight"] * searchDetails["inputPipelineRawScore"]);
    const searchScoreValue = searchDetails["value"];
    // If there isn't a details, we didn't get this back from search at all.
    if (searchDetails.hasOwnProperty("details")) {
        assertFieldPresent("details", searchDetails);
        assert.eq(searchDetails["details"], []);
        // Note we won't check the shape of the search scoreDetails beyond here.
    }

    const vectorDetails = subDetails[1];
    assertFieldPresent("inputPipelineName", vectorDetails);
    assert.eq(vectorDetails["inputPipelineName"], "vector");
    assertFieldPresent("inputPipelineRawScore", vectorDetails);
    if (i < inputPipeline1RawScoreExpectedResults.length) {
        assert.eq(vectorDetails["inputPipelineRawScore"],
                  inputPipeline1RawScoreExpectedResults[i]["score"]);
    }
    assertFieldPresent("weight", vectorDetails);
    assert.eq(vectorDetails["weight"], 1);
    assertFieldPresent("value", vectorDetails);
    assert.eq(vectorDetails["value"],
              vectorDetails["weight"] * vectorDetails["inputPipelineRawScore"]);
    const vectorScoreValue = vectorDetails["value"];
    assertFieldPresent("details", vectorDetails);
    assert.eq(vectorDetails["details"], []);
    assert.eq(score, (vectorScoreValue + searchScoreValue) / 2);
}

// Test search/vectorSearch where search scoreDetails is off and $scoreFusion's scoreDetails is off.
let testQuery = [
    {
        $scoreFusion: {
            input: {
                pipelines: {vector: [vectorStage], search: [searchStageNoDetails, {$limit: limit}]},
                normalization: "none"
            },
            scoreDetails: false,
        },
    },
    {$project: {score: {$meta: "score"}}}
];

results = coll.aggregate(testQuery).toArray();

/**
 * Example of the expected score metadata structure for a given results document:
 * "score" : 2.5521023273468018
 */
for (const foundDoc of results) {
    // Assert that the score metadata has been set.
    assert(fieldPresent("score", foundDoc), foundDoc);
    const score = foundDoc["score"];
    assert.gte(score, 0);
}

/**
 * Verify that when $scoreFusion.scoreDetails is false and an input pipeline ($search) has
 * scoreDetails set to true, the aggregation fails when scoreDetails metadata is projected out.
 */
testQuery = [
    {
        $scoreFusion: {
            input: {
                pipelines: {vector: [vectorStage], search: [searchStage, {$limit: limit}]},
                normalization: "none"
            },
            combination: {weights: {vector: 1, search: 2}},
            scoreDetails: false,
        },
    },
    {$project: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}}
];

assertErrCodeAndErrMsgContains(coll, testQuery, 40218, "query requires scoreDetails metadata");

/**
 * Verify that when $scoreFusion.scoreDetails is false and an input pipeline ($search) has
 * scoreDetails set to true, the aggregation succeeds when scoreDetails metadata is NOT projected
 * out.
 */
testQuery = [
    {
        $scoreFusion: {
            input: {pipelines: {search: [searchStage, {$limit: limit}]}, normalization: "none"},
            combination: {weights: {search: 2}},
            scoreDetails: false,
        },
    },
    {$project: {plot_embedding: 0}}
];

assert.commandWorked(db.runCommand({aggregate: collName, pipeline: testQuery, cursor: {}}));

dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});

coll.drop();
assert.commandWorked(coll.insertMany([
    {_id: 0, textField: "three blind mice", geoField: [23, 51]},
    {_id: 1, textField: "the three stooges", geoField: [25, 49]},
    {_id: 2, textField: "we three kings", geoField: [30, 51]}
]));
assert.commandWorked(coll.createIndex({geoField: "2d"}));

/**
 * Example of the expected score and scoreDetails metadata structure for a given results document.
 * "score": 1.499993680954223,
 * "details": {
        "value": 1.499993680954223,
        "description": "the value calculated by...",
        "normalization": "sigmoid", // other values: "none", "minMaxScaler"
        "combination": { // Example combination when the combination.method is "expression"
            "method": "custom expression",
            "expression": "{ string: { $add: [ { $multiply: [ \"$$scorePipe1\", 0.5 ] },
                \"$$scorePipe2\" ] } }"
        },
        "details": [
            {
                "inputPipelineName": "scorePipe1",
                "inputPipelineRawScore": 14.866068747318506,
                "weight": 1,
                "value": 0.9999996502576503,
                "details": []
            },
            {
                "inputPipelineName": "scorePipe2",
                "inputPipelineRawScore": 12,
                "weight": 1,
                "value": 0.9999938558253978,
                "details": []
            }
        ]
    }
 * @param normalization - Specify $scoreFusion.normalization (can be one of the following: 'none',
 * 'sigmoid' or 'minMaxScaler')
 * @param combinationMethod - Specify $scoreFusion.combinationMethod (can be 'average' or
 'expression')
 * @param scorePipeline1ScoreDetails - Specify $score.scoreDetails (for first input pipeline) to be
 * true or false.
 * @param scorePipeline2ScoreDetails - Specify $score.scoreDetails (for first second pipeline) to be
 * true or false.
 *
 * Runs a test query where $scoreFusion takes 2 input pipelines, each a $score stage. Validate that
 * the $scoreFusion's scoreDetails structure is accurate. Validates the top-level $scoreFusion
 * scoreDetails' fields and those of each individual input pipeline's scoreDetails (if its
 * scoreDetails is set to true).
 *
 * NOTE: If combination.method is "average" then $scoreFusion's combination will just be:
 *                  "combination": { "method": "average"}
 */

function checkScoreDetailsNormalizationCombinationMethod(
    normalization, combinationMethod, scorePipeline1ScoreDetails, scorePipeline2ScoreDetails) {
    const geoNear = {$geoNear: {near: [20, 40]}};
    const scoreGeoNearMetadata = {
        $score: {
            score: {$meta: "geoNearDistance"},
            normalizeFunction: "none",
            scoreDetails: scorePipeline1ScoreDetails
        }
    };
    const scoreAdd = {
        $score: {
            score: {$add: [10, 2]},
            normalizeFunction: "none",
            scoreDetails: scorePipeline2ScoreDetails
        }
    };
    let combination = {method: combinationMethod};
    if (combinationMethod === "expression") {
        combination['expression'] = {$add: [{$multiply: ["$$scorePipe1", 0.5]}, "$$scorePipe2"]};
    }
    const testQuery = (scoreDetails) => {
        let project = projectScoreScoreDetails;
        if (!scoreDetails) {
            project = projectScore;
        }
        let query = [
            {
                $scoreFusion: {
                    input: {
                        pipelines:
                            {scorePipe1: [geoNear, scoreGeoNearMetadata], scorePipe2: [scoreAdd]},
                        normalization: normalization
                    },
                    combination: combination,
                    scoreDetails: scoreDetails,
                },
            },
            project,
            sortAscending
        ];
        return query;
    };

    // Run original query with scoreDetails.
    results = coll.aggregate(testQuery(true)).toArray();

    // Run original query without scoreDetails.
    let resultsNoScoreDetails = coll.aggregate(testQuery(false)).toArray();

    // Run $scoreFusion's first pipeline input. We will use the score value it calculates to assert
    // that the calculated rawScore for the first input pipeline is correct.
    const inputPipeline1RawScoreExpectedResults =
        coll.aggregate([geoNear, scoreGeoNearMetadata, projectScore, sortAscending]).toArray();

    // Run $scoreFusion's second pipeline input. We will use the score value it calculates to assert
    // that the calculated rawScore for the second input pipeline is correct.
    const inputPipeline2RawScoreExpectedResults =
        coll.aggregate([scoreAdd, projectScore, sortAscending]).toArray();

    // We will use the score value it calculates to assert that the calculated
    // $scoreFusion.normalization applied to the first input pipeline is correct.
    let inputPipeline1NormalizedScoreExpectedResults;
    // We will use the score value it calculates to assert that the calculated
    // $scoreFusion.normalization applied to the second input pipeline is correct.
    let inputPipeline2NormalizedScoreExpectedResults;
    // Calculate expected normalization results for each input pipeline to $scoreFusion.
    if (normalization === "sigmoid") {
        const projectSigmoidScore = {$project: {score: {$sigmoid: {$meta: "score"}}}};
        // Run $scoreFusion's first pipeline input with normalization.
        inputPipeline1NormalizedScoreExpectedResults =
            coll.aggregate([geoNear, scoreGeoNearMetadata, projectSigmoidScore, sortAscending])
                .toArray();
        // Run $scoreFusion's second pipeline input with normalization.
        inputPipeline2NormalizedScoreExpectedResults =
            coll.aggregate([scoreAdd, projectSigmoidScore, sortAscending]).toArray();
    } else if (normalization === "minMaxScaler") {
        const minMaxScalerStage = {
            $setWindowFields: {
                sortBy: {_id: 1},
                output: {
                    "score": {
                        $minMaxScaler: {input: {$meta: "score"}, min: 0, max: 1},
                        window: {documents: ["unbounded", "unbounded"]}
                    },
                }
            }
        };
        // Run $scoreFusion's first pipeline input with normalization.
        inputPipeline1NormalizedScoreExpectedResults =
            coll.aggregate([geoNear, scoreGeoNearMetadata, minMaxScalerStage, sortAscending])
                .toArray();
        // Run $scoreFusion's second pipeline input with normalization.
        inputPipeline2NormalizedScoreExpectedResults =
            coll.aggregate([scoreAdd, minMaxScalerStage, sortAscending]).toArray();
    } else {
        throw 'passed an invalid normalization option to $scoreFusion';
    }

    /**
     *
     * @param assertFieldPresent - the assertFieldPresent function needs to be passed since it's
     *     defined inside the scope of the for loop
     * @param subDetails - the details for the input pipeline
     * @param pipelineName - the input pipeline's name
     * @param rawScore - the input pipeline's raw score
     * @param normalizedScore - the input pipeline's normalized score
     * @param scorePipelineScoreDetails - the input pipeline's score details
     * @param scoreScoreDetailsString - the input pipeline's $score string
     * @returns the final calculated score value for the input pipeline
     */
    function assertScoreFusionInputPipelineScoreDetails(assertFieldPresent,
                                                        subDetails,
                                                        pipelineName,
                                                        rawScore,
                                                        normalizedScore,
                                                        scorePipelineScoreDetails,
                                                        scoreScoreDetailsString) {
        assertFieldPresent("inputPipelineName", subDetails);
        assert.eq(subDetails["inputPipelineName"], pipelineName);
        assertFieldPresent("inputPipelineRawScore", subDetails);
        assert.eq(subDetails["inputPipelineRawScore"], rawScore["score"]);
        assertFieldPresent("weight", subDetails);
        assert.eq(subDetails["weight"], 1);
        assertFieldPresent("value", subDetails);  // Normalized + weighted score.
        const value = subDetails["value"];        // This is also the normalized value
                                                  // because the weight is 1.
        let inputPipelineNormalizedScore = value / 1;
        assert.eq(inputPipelineNormalizedScore, normalizedScore["score"]);

        // Asserts that the $score input pipeline's scoreDetails has the correct values for the
        // following fields: value, description, rawScore, normalization, weight, expression, and
        // details.
        function assertScoreScoreDetails(scoreDetailsDetails,
                                         inputPipelineRawScore,
                                         scoreNormalization,
                                         scoreWeight,
                                         scoreExpression) {
            assertFieldPresent("value", scoreDetailsDetails);
            assert.eq(scoreDetailsDetails["value"], inputPipelineRawScore);
            assertFieldPresent("description", scoreDetailsDetails);
            assert.eq(scoreDetailsDetails["description"], scoreDetailsDescription);
            assertFieldPresent("rawScore", scoreDetailsDetails);
            assertFieldPresent("normalization", scoreDetailsDetails);
            assert.eq(scoreDetailsDetails["normalization"], scoreNormalization);
            assertFieldPresent("weight", scoreDetailsDetails);
            assert.eq(scoreDetailsDetails["weight"], scoreWeight);
            assertFieldPresent("expression", scoreDetailsDetails);
            assert.eq(scoreDetailsDetails["expression"], scoreExpression);
            assertFieldPresent("details", scoreDetailsDetails);
            assert.eq(scoreDetailsDetails["details"], []);
        }
        if (scorePipelineScoreDetails) {
            assertFieldPresent("details", subDetails);
            const scoreDetailsDetails = subDetails["details"];
            assertScoreScoreDetails(scoreDetailsDetails,
                                    subDetails["inputPipelineRawScore"],
                                    "none",
                                    1,
                                    scoreScoreDetailsString);
        } else {
            assert.eq(subDetails["details"], []);
        }
        return value;
    }

    for (let i = 0; i < results.length; i++) {
        const foundDoc = results[i];
        // Assert that the score metadata has been set.
        assert(fieldPresent("score", foundDoc), foundDoc);
        const score = foundDoc["score"];
        assert(fieldPresent("details", foundDoc), foundDoc);
        const details = foundDoc["details"];
        assert(fieldPresent("value", details), details);
        // Assert that the score metadata is the same value as what scoreDetails set.
        assert.eq(details["value"], score);
        // Assert that the top-level value has the same value as the 'score' metadata which is set
        // when the same $scoreFusion pipeline is run without scoreDetails.
        assert.eq(details["value"], resultsNoScoreDetails[i]["score"]);
        assert(fieldPresent("description", details), details);
        assert.eq(details["description"], scoreFusionDetailsDescription);
        assert.eq(details["normalization"], normalization);
        const combination = details["combination"];
        assert(fieldPresent("method", combination), combination);
        if (combinationMethod === "expression") {
            assert.eq(combination["method"], "custom expression");
            assert(fieldPresent("expression", combination), combination);
            // Assert that the stringified custom expression is correct.
            assert.eq(
                combination["expression"],
                "{ string: { $add: [ { $multiply: [ '$$scorePipe1', 0.5 ] }, '$$scorePipe2' ] } }");
        } else if (combinationMethod === "avg") {
            assert.eq(combination["method"], "average");
        }
        function assertFieldPresent(field, obj) {
            assert(fieldPresent(field, obj),
                   `Looked for ${field} in ${tojson(obj)}. Full details: ${tojson(details)}`);
        }
        // Description of score fusion.
        assertFieldPresent("details", details);
        const subDetails = details["details"];
        assert.eq(subDetails.length, 2);

        const scoreDetails1 = subDetails[0];
        const value1 = assertScoreFusionInputPipelineScoreDetails(
            assertFieldPresent,
            scoreDetails1,
            "scorePipe1",
            inputPipeline1RawScoreExpectedResults[i],
            inputPipeline1NormalizedScoreExpectedResults[i],
            scorePipeline1ScoreDetails,
            "{ string: { $meta: 'geoNearDistance' } }");
        const scoreDetails2 = subDetails[1];
        const value2 = assertScoreFusionInputPipelineScoreDetails(
            assertFieldPresent,
            scoreDetails2,
            "scorePipe2",
            inputPipeline2RawScoreExpectedResults[i],
            inputPipeline2NormalizedScoreExpectedResults[i],
            scorePipeline2ScoreDetails,
            "{ string: { $add: [ 10.0, 2.0 ] } }");

        if (combinationMethod === "expression") {
            assert.eq((value1 * 0.5) + value2, score);
        } else if (combinationMethod === "avg") {
            assert.eq((value1 + value2) / 2, score);
        }
    }
}

// Tests with no scoreDetails enabled for either $score input pipeline.
checkScoreDetailsNormalizationCombinationMethod("sigmoid", "avg", false, false);
checkScoreDetailsNormalizationCombinationMethod("minMaxScaler", "avg", false, false);
checkScoreDetailsNormalizationCombinationMethod("sigmoid", "expression", false, false);
checkScoreDetailsNormalizationCombinationMethod("minMaxScaler", "expression", false, false);

// Tests with scoreDetails enabled for at least one $score input pipeline.
checkScoreDetailsNormalizationCombinationMethod("sigmoid", "expression", true, true);
checkScoreDetailsNormalizationCombinationMethod("minMaxScaler", "expression", true, true);
checkScoreDetailsNormalizationCombinationMethod("sigmoid", "expression", true, false);
checkScoreDetailsNormalizationCombinationMethod("sigmoid", "avg", false, true);
checkScoreDetailsNormalizationCombinationMethod("minMaxScaler", "expression", true, false);
checkScoreDetailsNormalizationCombinationMethod("minMaxScaler", "avg", false, true);
