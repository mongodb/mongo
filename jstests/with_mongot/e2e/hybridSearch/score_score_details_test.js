/*
 * Tests hybrid search $score score details. This test focuses on ensuring that the structure
 * and contents of the produced scoreDetails field is correct.
 *
 * @tags: [ featureFlagSearchHybridScoringFull, requires_fcv_82 ]
 */
import {
    // fieldPresent,
    checkScoreScoreDetails,
    scoreDetailsDescription,
} from "jstests/with_mongot/e2e_lib/hybrid_search_score_details_utils.js";

const coll = db[jsTestName()];

// Populates the collection with documents containing two fields: single and double. Field single
// has the range [1,10] and field double has the range [2,20].
coll.drop();
const bulk = coll.initializeUnorderedBulkOp();
const nDocs = 10;

for (let i = 1; i <= nDocs; i++) {
    bulk.insert({i, "single": i, "double": i * 2});
}
assert.commandWorked(bulk.execute());

// Test Explanation: Verify that the set scoreDetails for $score contains the correct fields and
// values when the normalize function is one of the following: "none", "sigmoid", or "minMaxScaler"
// The weight can be specified (must be between [0, 1]) or unspecified (defaults to 1.0).

/**
 * Example of the expected score and scoreDetails metadata structure for a given results document:
 * "score": 0.05555555555555555,
 * "details": {
 *      "value": 0.05555555555555555,
 *      "description": "the score calculated from...",
 *      "rawScore": 6,
 *      "normalization": "minMaxScaler", // ["none", "sigmoid", "minMaxScaler"]
 *      "weight": 0.5, // between [0,1]
 *      "details": []
 *  }
 */
function testScoreDetails(normalization, weight, expectedScoreResults) {
    let score = {
        score: {$add: ["$single", "$double"]},
        normalization: normalization,
        weight: weight,
        scoreDetails: true,
    };
    if (weight === "unspecified") {
        weight = 1.0;
        score = {
            score: {$add: ["$single", "$double"]},
            normalization: normalization,
            scoreDetails: true,
        };
    }
    let testQuery = [
        {
            $score: score,
        },
        {$addFields: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}},
        {$sort: {_id: 1}},
    ];

    let results = coll.aggregate(testQuery).toArray();

    for (let i = 0; i < results.length; i++) {
        const foundDoc = results[i];
        checkScoreScoreDetails(foundDoc, {
            value: expectedScoreResults[i]["expectedScore"] * weight,
            rawScore: foundDoc["single"] + foundDoc["double"],
            normalization: normalization,
            weight: weight,
            description: scoreDetailsDescription,
            expression: "{ string: { \$add: [ '$single', '$double' ] } }",
        });
    }
}

// Testcase: normalization is "none" and weight is 0.5.
// Pipeline returns an array of documents, each with the calculated expected score that
// $score should have computed.
let expectedScoreResults = coll
    .aggregate([{$project: {expectedScore: {$add: ["$single", "$double"]}}}, {$sort: {_id: 1}}])
    .toArray();
testScoreDetails("none", 0.5, expectedScoreResults);

// Testcase: normalization is "sigmoid" and weight is 0.2.
expectedScoreResults = coll
    .aggregate([{$project: {expectedScore: {$sigmoid: {$add: ["$single", "$double"]}}}}, {$sort: {_id: 1}}])
    .toArray();
testScoreDetails("sigmoid", 0.2, expectedScoreResults);

// Testcase: normalization is "minMaxScaler" and weight is 0.5.
expectedScoreResults = coll
    .aggregate([
        {
            $setWindowFields: {
                sortBy: {_id: 1},
                output: {
                    "expectedScore": {
                        $minMaxScaler: {input: {$add: ["$single", "$double"]}, min: 0, max: 1},
                        window: {documents: ["unbounded", "unbounded"]},
                    },
                },
            },
        },
        {$sort: {_id: 1}},
    ])
    .toArray();
testScoreDetails("minMaxScaler", 0.5, expectedScoreResults);

// Testcase: normalization is "minMaxScaler" and weight isn't specified.
expectedScoreResults = coll
    .aggregate([
        {
            $setWindowFields: {
                sortBy: {_id: 1},
                output: {
                    "expectedScore": {
                        $minMaxScaler: {input: {$add: ["$single", "$double"]}, min: 0, max: 1},
                        window: {documents: ["unbounded", "unbounded"]},
                    },
                },
            },
        },
        {$sort: {_id: 1}},
    ])
    .toArray();
testScoreDetails("minMaxScaler", "unspecified", expectedScoreResults);

// Testcase: multiple (recursive) $score stages results in the correct score details output.
(function testMultipleScoreStagesWithScoreDetails() {
    const normalizationMethods = ["none", "minMaxScaler", "sigmoid"];
    for (let firstScoreNormalization of normalizationMethods) {
        for (let secondScoreNormalization of normalizationMethods) {
            let firstScore = {
                score: "$single",
                normalization: firstScoreNormalization,
                scoreDetails: true,
            };

            let secondScore = {
                score: {$add: [{$meta: "score"}, 10]},
                normalization: secondScoreNormalization,
                scoreDetails: true,
            };

            let query = [
                {
                    $score: firstScore,
                },
                {$score: secondScore},
                {$addFields: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}},
                {$sort: {_id: 1}},
            ];

            let results = coll.aggregate(query).toArray();

            // Determine the stage specification for the rawScore in scoreDetails. It is calculated
            // by the output of the first $score stage (with normalization), and the raw score
            // calculation of the second $score stage (without normalization).
            let expectedResultsPipeline = [];
            if (firstScoreNormalization === "none") {
                expectedResultsPipeline.push({$addFields: {expected_raw_score: {$add: ["$single", 10]}}});
            } else if (firstScoreNormalization === "minMaxScaler") {
                expectedResultsPipeline.push({
                    $setWindowFields: {
                        sortBy: {_id: 1},
                        output: {
                            "normalized_first_score": {
                                $minMaxScaler: {input: "$single", min: 0, max: 1},
                                window: {documents: ["unbounded", "unbounded"]},
                            },
                        },
                    },
                });
                expectedResultsPipeline.push({
                    $addFields: {expected_raw_score: {$add: ["$normalized_first_score", 10]}},
                });
            } else if (firstScoreNormalization === "sigmoid") {
                expectedResultsPipeline.push({$addFields: {expected_raw_score: {$add: [{$sigmoid: "$single"}, 10]}}});
            }

            // Determine the stage specification to calculate the final score in scoreDetails. It is
            // calculated by taking the raw score output from the second $score and running it
            // through the normalization function of the second $score.
            if (secondScoreNormalization === "none") {
                expectedResultsPipeline.push({$addFields: {expected_final_score: "$expected_raw_score"}});
            } else if (secondScoreNormalization === "minMaxScaler") {
                expectedResultsPipeline.push({
                    $setWindowFields: {
                        sortBy: {_id: 1},
                        output: {
                            "expected_final_score": {
                                $minMaxScaler: {input: "$expected_raw_score", min: 0, max: 1},
                                window: {documents: ["unbounded", "unbounded"]},
                            },
                        },
                    },
                });
            } else if (secondScoreNormalization === "sigmoid") {
                expectedResultsPipeline.push({$addFields: {expected_final_score: {$sigmoid: "$expected_raw_score"}}});
            }

            // $project the fields we want in the expected results.
            expectedResultsPipeline.push({
                $project: {
                    _id: 1,
                    rawScore: "$expected_raw_score",
                    expectedScore: "$expected_final_score",
                },
            });
            expectedResultsPipeline.push({$sort: {_id: 1}});

            let expectedScoreResults = coll.aggregate(expectedResultsPipeline).toArray();

            for (let i = 0; i < results.length; i++) {
                const foundDoc = results[i];
                checkScoreScoreDetails(foundDoc, {
                    value: expectedScoreResults[i]["expectedScore"],
                    rawScore: expectedScoreResults[i]["rawScore"],
                });
            }
        }
    }
})();
