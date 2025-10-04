/*
 * Tests hybrid search $scoreFusion score details. This test focuses on ensuring that the structure
 * and contents of the produced scoreDetails field is correct.
 *
 * @tags: [ featureFlagSearchHybridScoringFull, requires_fcv_82 ]
 */
import {
    checkDefaultPipelineScoreDetails,
    checkOuterScoreDetails,
    fieldPresent,
    projectScoreAndScoreDetailsStage,
    projectScoreStage,
    scoreDetailsDescription,
    sortAscendingStage,
} from "jstests/with_mongot/e2e_lib/hybrid_search_score_details_utils.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(
    coll.insertMany([
        {_id: 0, textField: "three blind mice", geoField: [23, 51]},
        {_id: 1, textField: "the three stooges", geoField: [25, 49]},
        {_id: 2, textField: "we three kings", geoField: [30, 51]},
    ]),
);
assert.commandWorked(coll.createIndex({geoField: "2d"}));

/**
 * Example of the expected score and scoreDetails metadata structure for a given results document.
 * "score": 1.499993680954223,
 * "details": {
 *      "value": 1.499993680954223,
 *      "description": "the value calculated by...",
 *      "normalization": "sigmoid", // other values: "none", "minMaxScaler"
 *      "combination": { // Example combination when the combination.method is "expression"
 *          "method": "custom expression",
 *          "expression": "{ string: { $add: [ { $multiply: [ \"$$scorePipe1\", 0.5 ] },
 *              \"$$scorePipe2\" ] } }"
 *      },
 *      "details": [
 *          {
 *              "inputPipelineName": "scorePipe1",
 *              "inputPipelineRawScore": 14.866068747318506,
 *              "weight": 1,
 *              "value": 0.9999996502576503,
 *              "details": []
 *          },
 *          {
 *              "inputPipelineName": "scorePipe2",
 *              "inputPipelineRawScore": 12,
 *              "weight": 1,
 *              "value": 0.9999938558253978,
 *              "details": []
 *          }
 *      ]
 *  }
 * @param normalization - Specify $scoreFusion.normalization (can be one of the following: 'none',
 * 'sigmoid' or 'minMaxScaler')
 * @param combinationMethod - Specify $scoreFusion.combinationMethod (can be 'average' or
 * 'expression')
 * @param scorePipeline1IncludesScoreDetails - Specify $score.scoreDetails (for first input
 * pipeline) to be true or false.
 * @param scorePipeline2IncludesScoreDetails - Specify $score.scoreDetails (for first second
 * pipeline) to be true or false.
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
    normalization,
    combinationMethod,
    scorePipeline1IncludesScoreDetails,
    scorePipeline2IncludesScoreDetails,
    scorePipeline1Normalization,
    scorePipeline2Normalization,
) {
    const geoNear = {$geoNear: {near: [20, 40]}};
    const scoreGeoNearMetadata = {
        $score: {
            score: {$meta: "geoNearDistance"},
            normalization: scorePipeline1Normalization,
            scoreDetails: scorePipeline1IncludesScoreDetails,
        },
    };
    const scoreAdd = {
        $score: {
            score: {$add: [10, 2]},
            normalization: scorePipeline2Normalization,
            scoreDetails: scorePipeline2IncludesScoreDetails,
        },
    };
    let combination = {method: combinationMethod};
    if (combinationMethod === "expression") {
        combination["expression"] = {$add: [{$multiply: ["$$scorePipe1", 0.5]}, "$$scorePipe2"]};
    }
    const testQuery = (scoreDetails) => {
        let project = projectScoreAndScoreDetailsStage;
        if (!scoreDetails) {
            project = projectScoreStage;
        }
        let query = [
            {
                $scoreFusion: {
                    input: {
                        pipelines: {scorePipe1: [geoNear, scoreGeoNearMetadata], scorePipe2: [scoreAdd]},
                        normalization: normalization,
                    },
                    combination: combination,
                    scoreDetails: scoreDetails,
                },
            },
            project,
            sortAscendingStage,
        ];
        return query;
    };

    // Run original query with scoreDetails.
    let results = coll.aggregate(testQuery(true)).toArray();

    // Run original query without scoreDetails.
    let resultsNoScoreDetails = coll.aggregate(testQuery(false)).toArray();

    // Run $scoreFusion's input pipelines. We will use the score value it calculates to assert
    // that the calculated rawScore for the second input pipeline is correct. We avoid doing any
    // normalization that the inner $score stage would do.
    const inputPipeline1RawScoreExpectedResults = coll
        .aggregate([
            geoNear,
            {$score: {...scoreGeoNearMetadata.$score, normalization: "none"}},
            projectScoreStage,
            sortAscendingStage,
        ])
        .toArray();
    const inputPipeline2RawScoreExpectedResults = coll
        .aggregate([{$score: {...scoreAdd.$score, normalization: "none"}}, projectScoreStage, sortAscendingStage])
        .toArray();

    // Now run $scoreFusion's input pipelines with the inner $score stage's normalization
    // applied. We will use the score value it calculates to assert that the calculated
    // normalizedScore for the input pipelines is correct.
    const inputPipeline1InnerNormalizedScoreExpectedResults = coll
        .aggregate([geoNear, scoreGeoNearMetadata, projectScoreStage, sortAscendingStage])
        .toArray();
    const inputPipeline2InnerNormalizedScoreExpectedResults = coll
        .aggregate([scoreAdd, projectScoreStage, sortAscendingStage])
        .toArray();

    // We will use the score value it calculates to assert that the calculated
    // $scoreFusion.normalization applied to the first input pipeline is correct.
    let inputPipeline1ScoreFusionNormalizedScoreExpectedResults;
    // We will use the score value it calculates to assert that the calculated
    // $scoreFusion.normalization applied to the second input pipeline is correct.
    let inputPipeline2ScoreFusionNormalizedScoreExpectedResults;
    // Calculate expected normalization results for each input pipeline to $scoreFusion.
    if (normalization === "sigmoid") {
        const projectSigmoidScore = {$project: {score: {$sigmoid: {$meta: "score"}}}};
        // Run $scoreFusion's first pipeline input with normalization.
        inputPipeline1ScoreFusionNormalizedScoreExpectedResults = coll
            .aggregate([geoNear, scoreGeoNearMetadata, projectSigmoidScore, sortAscendingStage])
            .toArray();
        // Run $scoreFusion's second pipeline input with normalization.
        inputPipeline2ScoreFusionNormalizedScoreExpectedResults = coll
            .aggregate([scoreAdd, projectSigmoidScore, sortAscendingStage])
            .toArray();
    } else if (normalization === "minMaxScaler") {
        const minMaxScalerStage = {
            $setWindowFields: {
                sortBy: {_id: 1},
                output: {
                    "score": {
                        $minMaxScaler: {input: {$meta: "score"}, min: 0, max: 1},
                        window: {documents: ["unbounded", "unbounded"]},
                    },
                },
            },
        };
        // Run $scoreFusion's first pipeline input with normalization.
        inputPipeline1ScoreFusionNormalizedScoreExpectedResults = coll
            .aggregate([geoNear, scoreGeoNearMetadata, minMaxScalerStage, sortAscendingStage])
            .toArray();
        // Run $scoreFusion's second pipeline input with normalization.
        inputPipeline2ScoreFusionNormalizedScoreExpectedResults = coll
            .aggregate([scoreAdd, minMaxScalerStage, sortAscendingStage])
            .toArray();
    } else {
        throw "passed an invalid normalization option to $scoreFusion";
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
    function assertScoreFusionInputPipelineScoreDetails(
        assertFieldPresent,
        generatedScoreDetailsOfInputPipeline,
        pipelineName,
        rawScore,
        innerNormalizedScore,
        normalizedScore,
        scorePipelineIncludesScoreDetails,
        scoreScoreDetailsString,
        scorePipelineNormalizationMethod,
    ) {
        checkDefaultPipelineScoreDetails(
            "score",
            assertFieldPresent,
            generatedScoreDetailsOfInputPipeline,
            pipelineName,
            1,
        );
        assertFieldPresent("inputPipelineRawScore", generatedScoreDetailsOfInputPipeline);
        assert.eq(generatedScoreDetailsOfInputPipeline["inputPipelineRawScore"], innerNormalizedScore["score"]);
        assertFieldPresent("value", generatedScoreDetailsOfInputPipeline); // Normalized + weighted score.
        const inputPipelineScoreValue = generatedScoreDetailsOfInputPipeline["value"]; // This is also the normalized value
        // because the weight is 1.
        let inputPipelineNormalizedScore = inputPipelineScoreValue / 1;
        assert.eq(inputPipelineNormalizedScore, normalizedScore["score"]);

        // Assert that the $score input pipeline's scoreDetails has the correct values for the
        // following fields: value, description, rawScore, normalization, weight, expression, and
        // details.
        if (scorePipelineIncludesScoreDetails) {
            assertFieldPresent("details", generatedScoreDetailsOfInputPipeline);
            let subComparisonBlob = {
                "value": innerNormalizedScore["score"],
                "description": scoreDetailsDescription,
                "rawScore": rawScore["score"],
                "normalization": scorePipelineNormalizationMethod,
                "weight": 1,
                "expression": scoreScoreDetailsString,
                "details": [],
            };
            assert.eq(generatedScoreDetailsOfInputPipeline["details"], subComparisonBlob);
        } else {
            assert.eq(generatedScoreDetailsOfInputPipeline["details"], []);
        }
        return inputPipelineScoreValue;
    }

    // Assert that the top-level fields (not the input pipeline details) of the document's
    // scoreDetails are accurate.
    for (let i = 0; i < results.length; i++) {
        const foundDoc = results[i];
        checkOuterScoreDetails(foundDoc, {
            stageType: "score",
            numInputPipelines: 2,
            normalization: normalization,
            combinationMethod: combinationMethod,
            combinationExpressionString:
                combinationMethod === "expression"
                    ? "{ string: { \$add: [ { \$multiply: [ '$$scorePipe1', 0.5 ] }, " + "'$$scorePipe2' ] } }"
                    : null,
        });

        const score = foundDoc["score"];
        const details = foundDoc["details"];
        function assertFieldPresent(field, obj) {
            assert(fieldPresent(field, obj), `Looked for ${field} in ${tojson(obj)}. Full details: ${tojson(details)}`);
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
            inputPipeline1InnerNormalizedScoreExpectedResults[i],
            inputPipeline1ScoreFusionNormalizedScoreExpectedResults[i],
            scorePipeline1IncludesScoreDetails,
            "{ string: { $meta: 'geoNearDistance' } }",
            scorePipeline1Normalization,
        );
        const inputPipeline2ScoreDetails = subDetails[1];
        const inputPipeline2ScoreValue = assertScoreFusionInputPipelineScoreDetails(
            assertFieldPresent,
            inputPipeline2ScoreDetails,
            "scorePipe2",
            inputPipeline2RawScoreExpectedResults[i],
            inputPipeline2InnerNormalizedScoreExpectedResults[i],
            inputPipeline2ScoreFusionNormalizedScoreExpectedResults[i],
            scorePipeline2IncludesScoreDetails,
            "{ string: { $add: [ 10.0, 2.0 ] } }",
            scorePipeline2Normalization,
        );

        // Assert that the final $scoreFusion score output is properly calculated as a combination
        // of the input pipeline scores.
        if (combinationMethod === "expression") {
            // Original combination expression is: {$add: [{$multiply: ["$$scorePipe1", 0.5]},
            // "$$scorePipe2"]}
            assert.eq(inputPipeline1ScoreValue * 0.5 + inputPipeline2ScoreValue, score);
        } else if (combinationMethod === "avg") {
            assert.eq((inputPipeline1ScoreValue + inputPipeline2ScoreValue) / 2, score);
        }
    }
}

const normalizationMethods = ["sigmoid", "minMaxScaler"];
const combinationMethods = ["expression", "avg"];
const inputPipelineNormalizationMethods = ["none", "sigmoid", "minMaxScaler"];

for (let normalizationMethod of normalizationMethods) {
    for (let combinationMethod of combinationMethods) {
        for (let scoreDetailsOnFirstInputPipeline of [true, false]) {
            for (let scoreDetailsOnSecondInputPipeline of [true, false]) {
                for (let firstInputPipelineNormalization of inputPipelineNormalizationMethods) {
                    for (let secondInputPipelineNormalization of inputPipelineNormalizationMethods) {
                        checkScoreDetailsNormalizationCombinationMethod(
                            normalizationMethod,
                            combinationMethod,
                            scoreDetailsOnFirstInputPipeline,
                            scoreDetailsOnSecondInputPipeline,
                            firstInputPipelineNormalization,
                            secondInputPipelineNormalization,
                        );
                    }
                }
            }
        }
    }
}
