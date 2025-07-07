/*
 * Tests hybrid search $scoreFusion score details. This test focuses on ensuring that the structure
 * and contents of the produced scoreDetails field is correct.
 *
 * @tags: [ featureFlagSearchHybridScoringFull, requires_fcv_81 ]
 */

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

const projectScoreScoreDetails = {
    $project: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}
};

const sortAscending = {
    $sort: {_id: 1}
};

const projectScore = {
    $project: {score: {$meta: "score"}}
};

function fieldPresent(field, containingObj) {
    return containingObj.hasOwnProperty(field);
}

const scoreFusionDetailsDescription =
    "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:";

const scoreDetailsDescription =
    "the score calculated from multiplying a weight in the range [0,1] with either a normalized or nonnormalized value:";

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
 * @param scorePipeline1IncludesScoreDetails - Specify $score.scoreDetails (for first input
 pipeline) to be
 * true or false.
 * @param scorePipeline2IncludesScoreDetails - Specify $score.scoreDetails (for first second
 pipeline) to be
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

function checkScoreDetailsNormalizationCombinationMethod(normalization,
                                                         combinationMethod,
                                                         scorePipeline1IncludesScoreDetails,
                                                         scorePipeline2IncludesScoreDetails,
                                                         scorePipeline1Normalization,
                                                         scorePipeline2Normalization) {
    const geoNear = {$geoNear: {near: [20, 40]}};
    const scoreGeoNearMetadata = {
        $score: {
            score: {$meta: "geoNearDistance"},
            normalization: scorePipeline1Normalization,
            scoreDetails: scorePipeline1IncludesScoreDetails
        }
    };
    const scoreAdd = {
        $score: {
            score: {$add: [10, 2]},
            normalization: scorePipeline2Normalization,
            scoreDetails: scorePipeline2IncludesScoreDetails
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
    let results = coll.aggregate(testQuery(true)).toArray();

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
     * Asserts that the generated scoreDetails for a $scoreFusion input pipeline (that is, the
     * sub-details for that input pipeline inside the total $scoreFusion scoreDetails object)
     * contains the expected values.
     *
     * @param assertFieldPresent - the assertFieldPresent function needs to be passed since it's
     *     defined inside the scope of the for loop
     * @param generatedScoreDetailsOfInputPipeline - the details for the input pipeline
     * @param pipelineName - the input pipeline's name
     * @param rawScore - the input pipeline's raw score
     * @param normalizedScore - the input pipeline's normalized score
     * @param scorePipelineIncludesScoreDetails - whether or not the input pipeline's score details
     * @param scoreScoreDetailsString - the input pipeline's $score string
     * @returns the final calculated score value for the input pipeline
     */
    function assertScoreFusionInputPipelineScoreDetails(assertFieldPresent,
                                                        generatedScoreDetailsOfInputPipeline,
                                                        pipelineName,
                                                        rawScore,
                                                        normalizedScore,
                                                        scorePipelineIncludesScoreDetails,
                                                        scoreScoreDetailsString,
                                                        scorePipelineNormalizationMethod) {
        assertFieldPresent("inputPipelineName", generatedScoreDetailsOfInputPipeline);
        assert.eq(generatedScoreDetailsOfInputPipeline["inputPipelineName"], pipelineName);
        assertFieldPresent("inputPipelineRawScore", generatedScoreDetailsOfInputPipeline);
        assert.eq(generatedScoreDetailsOfInputPipeline["inputPipelineRawScore"], rawScore["score"]);
        assertFieldPresent("weight", generatedScoreDetailsOfInputPipeline);
        assert.eq(generatedScoreDetailsOfInputPipeline["weight"], 1);
        assertFieldPresent("value",
                           generatedScoreDetailsOfInputPipeline);  // Normalized + weighted score.
        const inputPipelineScoreValue =
            generatedScoreDetailsOfInputPipeline["value"];  // This is also the normalized value
                                                            // because the weight is 1.
        let inputPipelineNormalizedScore = inputPipelineScoreValue / 1;
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
        if (scorePipelineIncludesScoreDetails) {
            assertFieldPresent("details", generatedScoreDetailsOfInputPipeline);
            const scoreDetailsDetails = generatedScoreDetailsOfInputPipeline["details"];
            assertScoreScoreDetails(scoreDetailsDetails,
                                    generatedScoreDetailsOfInputPipeline["inputPipelineRawScore"],
                                    scorePipelineNormalizationMethod,
                                    1,
                                    scoreScoreDetailsString);
        } else {
            assert.eq(generatedScoreDetailsOfInputPipeline["details"], []);
        }
        return inputPipelineScoreValue;
    }

    // Assert that the top-level fields (not the input pipeline details) of the document's
    // scoreDetails are accurate.
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

        // Assert that the scoreDetails of each of the 2 input pipelines are accurate.
        const inputPipeline1ScoreDetails = subDetails[0];
        const inputPipeline1ScoreValue = assertScoreFusionInputPipelineScoreDetails(
            assertFieldPresent,
            inputPipeline1ScoreDetails,
            "scorePipe1",
            inputPipeline1RawScoreExpectedResults[i],
            inputPipeline1NormalizedScoreExpectedResults[i],
            scorePipeline1IncludesScoreDetails,
            "{ string: { $meta: 'geoNearDistance' } }",
            scorePipeline1Normalization);
        const inputPipeline2ScoreDetails = subDetails[1];
        const inputPipeline2ScoreValue = assertScoreFusionInputPipelineScoreDetails(
            assertFieldPresent,
            inputPipeline2ScoreDetails,
            "scorePipe2",
            inputPipeline2RawScoreExpectedResults[i],
            inputPipeline2NormalizedScoreExpectedResults[i],
            scorePipeline2IncludesScoreDetails,
            "{ string: { $add: [ 10.0, 2.0 ] } }",
            scorePipeline2Normalization);

        // Assert that the final $scoreFusion score output is properly calculated as a combination
        // of the input pipeline scores.
        if (combinationMethod === "expression") {
            // Original combination expression is: {$add: [{$multiply: ["$$scorePipe1", 0.5]},
            // "$$scorePipe2"]}
            assert.eq((inputPipeline1ScoreValue * 0.5) + inputPipeline2ScoreValue, score);
        } else if (combinationMethod === "avg") {
            assert.eq((inputPipeline1ScoreValue + inputPipeline2ScoreValue) / 2, score);
        }
    }
}

const normalizationMethods = ["sigmoid", "minMaxScaler"];
const combinationMethods = ["expression", "avg"];
const inputPipelineNormalizationMethods = ["none", "sigmoid", "minMaxScaler"];

for (var normalizationMethod of normalizationMethods) {
    for (var combinationMethod of combinationMethods) {
        for (var scoreDetailsOnFirstInputPipeline of [true, false]) {
            for (var scoreDetailsOnSecondInputPipeline of [true, false]) {
                for (var firstInputPipelineNormalization of inputPipelineNormalizationMethods) {
                    for (var secondInputPipelineNormalization of
                             inputPipelineNormalizationMethods) {
                        checkScoreDetailsNormalizationCombinationMethod(
                            normalizationMethod,
                            combinationMethod,
                            scoreDetailsOnFirstInputPipeline,
                            scoreDetailsOnSecondInputPipeline,
                            firstInputPipelineNormalization,
                            secondInputPipelineNormalization);
                    }
                }
            }
        }
    }
}
