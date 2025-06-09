/*
 * Tests hybrid search $score score details. This test focuses on ensuring that the structure
 * and contents of the produced scoreDetails field is correct.
 *
 * @tags: [ featureFlagSearchHybridScoringFull, requires_fcv_81 ]
 */

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

function fieldPresent(field, containingObj) {
    return containingObj.hasOwnProperty(field);
}

const scoreDetailsDescription =
    "the score calculated from multiplying a weight in the range [0,1] with either a normalized or nonnormalized value:";

// Test Explanation: Verify that the set scoreDetails for $score contains the correct fields and
// values when the normalize function is one of the following: "none", "sigmoid", or "minMaxScaler"
// The weight can be specified (must be between [0, 1]) or unspecified (defaults to 1.0).

/**
 * Example of the expected score and scoreDetails metadata structure for a given results document:
 * "score": 0.05555555555555555,
 * "details": {
        "value": 0.05555555555555555,
        "description": "the score calculated from...",
        "rawScore": 6,
        "normalization": "minMaxScaler", // ["none", "sigmoid", "minMaxScaler"]
        "weight": 0.5, // between [0,1]
        "details": []
    }
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
        {$sort: {_id: 1}}
    ];

    let results = coll.aggregate(testQuery).toArray();

    for (let i = 0; i < results.length; i++) {
        const foundDoc = results[i];
        // Assert that the score metadata has been set.
        assert(fieldPresent("score", foundDoc), foundDoc);
        const score = foundDoc["score"];
        assert.eq(score, expectedScoreResults[i]["expectedScore"] * weight);
        assert(fieldPresent("details", foundDoc), foundDoc);
        const details = foundDoc["details"];
        assert(fieldPresent("value", details), details);
        // We don't care about the actual score, just assert that its been calculated.
        assert.gte(details["value"], 0, details);
        // Assert that the score metadata is the same value as what scoreDetails set.
        assert.eq(details["value"], score);
        assert(fieldPresent("description", details), details);
        assert.eq(details["description"], scoreDetailsDescription);
        assert(fieldPresent("rawScore", details), details);
        assert.eq(details["rawScore"], foundDoc["single"] + foundDoc["double"]);
        assert((fieldPresent["normalization"], details), details);
        assert.eq(details["normalization"], normalization);
        assert(fieldPresent("weight", details), details);
        assert.eq(details["weight"], weight);
        assert(fieldPresent("expression", details), details);
        assert.eq(details["expression"], "{ string: { $add: [ '$single', '$double' ] } }");
        assert(fieldPresent("details", details), details);
        assert.eq(details["details"], []);
    }
}

// Testcase: normalization is "none" and weight is 0.5.
// Pipeline returns an array of documents, each with the calculated expected score that
// $score should have computed.
let expectedScoreResults =
    coll.aggregate([{$project: {expectedScore: {$add: ["$single", "$double"]}}}, {$sort: {_id: 1}}])
        .toArray();
testScoreDetails("none", 0.5, expectedScoreResults);

// Testcase: normalization is "sigmoid" and weight is 0.2.
expectedScoreResults =
    coll.aggregate([
            {$project: {expectedScore: {$sigmoid: {$add: ["$single", "$double"]}}}},
            {$sort: {_id: 1}}
        ])
        .toArray();
testScoreDetails("sigmoid", 0.2, expectedScoreResults);

// Testcase: normalization is "minMaxScaler" and weight is 0.5.
expectedScoreResults =
    coll.aggregate([
            {
                $setWindowFields: {
                    sortBy: {_id: 1},
                    output: {
                        "expectedScore": {
                            $minMaxScaler: {input: {$add: ["$single", "$double"]}, min: 0, max: 1},
                            window: {documents: ["unbounded", "unbounded"]}
                        },
                    }
                }
            },
            {$sort: {_id: 1}}
        ])
        .toArray();
testScoreDetails("minMaxScaler", 0.5, expectedScoreResults);

// Testcase: normalization is "minMaxScaler" and weight isn't specified.
expectedScoreResults =
    coll.aggregate([
            {
                $setWindowFields: {
                    sortBy: {_id: 1},
                    output: {
                        "expectedScore": {
                            $minMaxScaler: {input: {$add: ["$single", "$double"]}, min: 0, max: 1},
                            window: {documents: ["unbounded", "unbounded"]}
                        },
                    }
                }
            },
            {$sort: {_id: 1}}
        ])
        .toArray();
testScoreDetails("minMaxScaler", "unspecified", expectedScoreResults);
