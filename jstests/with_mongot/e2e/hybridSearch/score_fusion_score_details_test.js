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

const scoreFusionDetailsDescription =
    "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:";

// Test search/vectorSearch where only search has scoreDetails.
let testQuery = [
    {
        $scoreFusion: {
            input: {
                pipelines: {vector: [vectorStage], search: [searchStage, {$limit: limit}]},
                normalization: "none"
            },
            combination: {weights: {vector: 1, search: 2}},
            scoreDetails: true,
        },
    },
    {$project: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}}
];

let results = coll.aggregate(testQuery).toArray();

function fieldPresent(field, containingObj) {
    return containingObj.hasOwnProperty(field);
}

/**
 * Example of the expected score and scoreDetails metadata structure for a given results document:
 * "score" : 4.1042046546936035,
 * "details" : {
        "value" : 4.1042046546936035,
        "description" : "the value calculated by...",
        "normalization" : "none",
        "combination": {
            "method" : "sum"
        },
        "details" : [
            {
                "inputPipelineName" : "search",
                "inputPipelineRawScore" : 1.5521023273468018,
                "weight" : 2,
                "value" : 1.5521023273468018,
                "description" : "sum of:",
                "details" : [ {...} ]
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
for (const foundDoc of results) {
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
    assert.eq(details["description"], scoreFusionDetailsDescription);
    assert.eq(details["normalization"], "none");
    const combination = details["combination"];
    assert(fieldPresent("method", combination), combination);
    assert.eq(combination["method"], "sum");

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
    assertFieldPresent("weight", searchDetails);
    assert.eq(searchDetails["weight"], 2);
    // If there isn't a value, we didn't get this back from search at all.
    if (searchDetails.hasOwnProperty("value")) {
        assertFieldPresent("value", searchDetails);
        assertFieldPresent("details",
                           searchDetails);  // Not checking description contents, just that its
                                            // present and not our placeholder value.
        assert.neq(searchDetails["details"], []);
        // Note we won't check the shape of the search scoreDetails beyond here.
    }

    const vectorDetails = subDetails[1];
    assertFieldPresent("inputPipelineName", vectorDetails);
    assert.eq(vectorDetails["inputPipelineName"], "vector");
    assertFieldPresent("inputPipelineRawScore", vectorDetails);
    assertFieldPresent("weight", vectorDetails);
    assert.eq(vectorDetails["weight"], 1);
    assertFieldPresent("details", vectorDetails);
    assert.eq(vectorDetails["details"], []);
}

// Test vectorSearch/vectorSearch where neither has score details.
testQuery = [
    {
        $scoreFusion: {
            input: {
                pipelines: {vector: [vectorStage], secondVector: [vectorStage]},
                normalization: "none"
            },
            combination: {weights: {vector: 0.5, secondVector: 2.8}},
            scoreDetails: true,
        },
    },
    {$project: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}}
];
results = coll.aggregate(testQuery).toArray();

/**
 * Example of the expected score and scoreDetails metadata structure for a given results document:
 * "score" : 3.3,
 * "details" : {
        "value" : 3.3,
        "description" : "the value calculated by...",
        "normalization" : "none",
        "combination" : {
            "method" : "sum"
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
for (const foundDoc of results) {
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
    assert(fieldPresent("description", details), details);
    assert.eq(details["description"], scoreFusionDetailsDescription);
    assert.eq(details["normalization"], "none");
    const combination = details["combination"];
    assert(fieldPresent("method", combination), combination);
    assert.eq(combination["method"], "sum");

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
    const inputPipeline2RawScore = secondVectorDetails["inputPipelineRawScore"];
    assertFieldPresent("weight", secondVectorDetails);
    assert.eq(secondVectorDetails["weight"], 2.8);
    assertFieldPresent("value", secondVectorDetails);  // Original 'score' AKA vectorSearchScore.
    assert.eq(secondVectorDetails["value"], inputPipeline2RawScore * 2.8);
    assertFieldPresent("details",
                       secondVectorDetails);  // Not checking description contents, just that its
                                              // present and not our placeholder value.
    assert.eq(secondVectorDetails["details"], []);

    const vectorDetails = subDetails[1];
    assertFieldPresent("inputPipelineName", vectorDetails);
    assert.eq(vectorDetails["inputPipelineName"], "vector");
    assertFieldPresent("inputPipelineRawScore", vectorDetails);
    const inputPipeline1RawScore = vectorDetails["inputPipelineRawScore"];
    assertFieldPresent("weight", vectorDetails);
    assert.eq(vectorDetails["weight"], 0.5);
    assertFieldPresent("value", vectorDetails);  // Original 'score' AKA vectorSearchScore.
    assert.eq(vectorDetails["value"], inputPipeline1RawScore * 0.5);
    assertFieldPresent("details", vectorDetails);
    assert.eq(vectorDetails["details"], []);
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

testQuery = [
    {
        $scoreFusion: {
            input: {
                pipelines: {vector: [vectorStage], search: [searchStageNoDetails, {$limit: limit}]},
                normalization: "none"
            },
            scoreDetails: true,
        },
    },
    {$project: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}}
];

results = coll.aggregate(testQuery).toArray();

/**
 * Example of the expected score and scoreDetails metadata structure for a given results document:
 * "score" : 2.5521023273468018,
 * "details" : {
        "value" : 2.5521023273468018,
        "description" : "the value calculated by...",
        "normalization" : "none",
        "combination" : {
            "method" : "sum"
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
for (const foundDoc of results) {
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
    assert(fieldPresent("description", details), details);
    assert.eq(details["description"], scoreFusionDetailsDescription);
    assert.eq(details["normalization"], "none");
    const combination = details["combination"];
    assert(fieldPresent("method", combination), combination);
    assert.eq(combination["method"], "sum");

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
    assertFieldPresent("weight", searchDetails);
    assert.eq(searchDetails["weight"], 1);
    // If there isn't a value, we didn't get this back from search at all.
    if (searchDetails.hasOwnProperty("value")) {
        assertFieldPresent("value", searchDetails);
        assertFieldPresent("details", searchDetails);
        assert.eq(searchDetails["details"], []);
        // Note we won't check the shape of the search scoreDetails beyond here.
    }

    const vectorDetails = subDetails[1];
    assertFieldPresent("inputPipelineName", vectorDetails);
    assert.eq(vectorDetails["inputPipelineName"], "vector");
    assertFieldPresent("inputPipelineRawScore", vectorDetails);
    assertFieldPresent("weight", vectorDetails);
    assert.eq(vectorDetails["weight"], 1);
    assertFieldPresent("details", vectorDetails);
    assert.eq(vectorDetails["details"], []);
}

// Test search/vectorSearch where search scoreDetails is off and $scoreFusion's scoreDetails is off.
testQuery = [
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

// TODO SERVER-94602 Test scoreDetails with nested scoreFusion.
dropSearchIndex(coll, {name: getMovieSearchIndexSpec().name});
dropSearchIndex(coll, {name: getMovieVectorSearchIndexSpec().name});

coll.drop();
assert.commandWorked(coll.insertMany([
    {_id: 0, textField: "three blind mice", geoField: [23, 51]},
    {_id: 1, textField: "the three stooges", geoField: [25, 49]},
    {_id: 2, textField: "we three kings", geoField: [30, 51]}
]));
assert.commandWorked(coll.createIndex({geoField: "2d"}));

testQuery = [
    {
        $scoreFusion: {
            input: {
                pipelines: {
                    scorePipe1: [
                        {$geoNear: {near: [20, 40]}},
                        {$score: {score: {$meta: "geoNearDistance"}, normalizeFunction: "none"}}
                    ],
                    scorePipe2: [{$score: {score: {$add: [10, 2]}, normalizeFunction: "none"}}]
                },
                normalization: "sigmoid"
            },
            combination: {method: "avg"},
            scoreDetails: true,
        },
    },
    {$project: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}}
];

results = coll.aggregate(testQuery).toArray();

/**
 * Example of the expected score and scoreDetails metadata structure for a given results document:
 * "score": 0.999996753041524,
 * "details": {
        "value": 0.999996753041524,
        "description": "the value calculated by...",
        "normalization": "sigmoid",
        "combination": {
            "method": "average"
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
 */
for (const foundDoc of results) {
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
    assert(fieldPresent("description", details), details);
    assert.eq(details["description"], scoreFusionDetailsDescription);
    assert.eq(details["normalization"], "sigmoid");
    const combination = details["combination"];
    assert(fieldPresent("method", combination), combination);
    assert.eq(combination["method"], "average");

    function assertFieldPresent(field, obj) {
        assert(fieldPresent(field, obj),
               `Looked for ${field} in ${tojson(obj)}. Full details: ${tojson(details)}`);
    }
    // Description of score fusion.
    assertFieldPresent("details", details);
    const subDetails = details["details"];
    assert.eq(subDetails.length, 2);

    const scoreDetails1 = subDetails[0];
    assertFieldPresent("inputPipelineName", scoreDetails1);
    assert.eq(scoreDetails1["inputPipelineName"], "scorePipe1");
    assertFieldPresent("inputPipelineRawScore", scoreDetails1);
    assertFieldPresent("weight", scoreDetails1);
    assert.eq(scoreDetails1["weight"], 1);
    assertFieldPresent("value", scoreDetails1);  // Normalized + weighted score.
    assertFieldPresent("details",
                       scoreDetails1);  // Not checking description contents, just that its
                                        // present and not our placeholder value.
    assert.eq(scoreDetails1["details"], []);

    const scoreDetails2 = subDetails[1];
    assertFieldPresent("inputPipelineName", scoreDetails2);
    assert.eq(scoreDetails2["inputPipelineName"], "scorePipe2");
    assertFieldPresent("inputPipelineRawScore", scoreDetails2);
    assert.eq(scoreDetails2["inputPipelineRawScore"], 12);
    assertFieldPresent("weight", scoreDetails2);
    assert.eq(scoreDetails2["weight"], 1);
    assertFieldPresent("value", scoreDetails2);  // Normalized + weighted score.
    assert.gt(details["value"], 0);
    assert.lt(details["value"], 1);
    assertFieldPresent("details",
                       scoreDetails2);  // Not checking description contents, just that its
                                        // present and not our placeholder value.
    assert.eq(scoreDetails2["details"], []);
}

testQuery = [
    {
        $scoreFusion: {
            input: {
                pipelines: {
                    scorePipe1: [
                        {$geoNear: {near: [20, 40]}},
                        {$score: {score: {$meta: "geoNearDistance"}, normalizeFunction: "none"}}
                    ],
                    scorePipe2: [{$score: {score: {$add: [10, 2]}, normalizeFunction: "none"}}]
                },
                normalization: "sigmoid"
            },
            combination: {
                method: "expression",
                expression: {$add: [{$multiply: ["$$scorePipe1", 0.5]}, "$$scorePipe2"]}
            },
            scoreDetails: true,
        },
    },
    {$project: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}}
];

results = coll.aggregate(testQuery).toArray();

const exprString =
    "{ string: { $add: [ { $multiply: [ '$$scorePipe1', 0.5 ] }, '$$scorePipe2' ] } }";

/**
 * Example of the expected score and scoreDetails metadata structure for a given results document:
 * "score": 1.499993680954223,
 * "details": {
        "value": 1.499993680954223,
        "description": "the value calculated by...",
        "normalization": "sigmoid",
        "combination": {
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
 */
for (const foundDoc of results) {
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
    assert(fieldPresent("description", details), details);
    assert.eq(details["description"], scoreFusionDetailsDescription);
    assert.eq(details["normalization"], "sigmoid");
    const combination = details["combination"];
    assert(fieldPresent("method", combination), combination);
    assert.eq(combination["method"], "custom expression");
    assert(fieldPresent("expression", combination), combination);
    assert.eq(combination["expression"], exprString);

    function assertFieldPresent(field, obj) {
        assert(fieldPresent(field, obj),
               `Looked for ${field} in ${tojson(obj)}. Full details: ${tojson(details)}`);
    }
    // Description of score fusion.
    assertFieldPresent("details", details);
    const subDetails = details["details"];
    assert.eq(subDetails.length, 2);

    const scoreDetails1 = subDetails[0];
    assertFieldPresent("inputPipelineName", scoreDetails1);
    assert.eq(scoreDetails1["inputPipelineName"], "scorePipe1");
    assertFieldPresent("inputPipelineRawScore", scoreDetails1);
    assertFieldPresent("weight", scoreDetails1);
    assert.eq(scoreDetails1["weight"], 1);
    assertFieldPresent("value", scoreDetails1);  // Normalized + weighted score.
    const value1 = scoreDetails1["value"];
    assertFieldPresent("details",
                       scoreDetails1);  // Not checking description contents, just that its
                                        // present and not our placeholder value.
    assert.eq(scoreDetails1["details"], []);

    const scoreDetails2 = subDetails[1];
    assertFieldPresent("inputPipelineName", scoreDetails2);
    assert.eq(scoreDetails2["inputPipelineName"], "scorePipe2");
    assertFieldPresent("inputPipelineRawScore", scoreDetails2);
    assert.eq(scoreDetails2["inputPipelineRawScore"], 12);
    assertFieldPresent("weight", scoreDetails2);
    assert.eq(scoreDetails2["weight"], 1);
    assertFieldPresent("value", scoreDetails2);  // Normalized + weighted score.
    assert.gt(scoreDetails2["value"], 0);
    assert.lt(scoreDetails2["value"], 1);
    const value2 = scoreDetails2["value"];
    assertFieldPresent("details",
                       scoreDetails2);  // Not checking description contents, just that its
                                        // present and not our placeholder value.
    assert.eq(scoreDetails2["details"], []);
    assert.eq((value1 * 0.5) + value2, score);
}
