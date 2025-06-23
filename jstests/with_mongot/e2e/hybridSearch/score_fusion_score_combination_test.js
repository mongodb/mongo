/**
 * Tests that the $scoreFusion.combination.method works as expected.
 * @tags: [ featureFlagRankFusionFull, featureFlagSearchHybridScoringFull, requires_fcv_81 ]
 */

/**
 * Helper function that runs a basic aggregate command with the specified pipeline and no other
 * command-level vars.
 * @param {Object[]} pipeline
 */
function runPipeline(pipeline) {
    return db.runCommand({aggregate: jsTestName(), pipeline, cursor: {}});
}

/**
 * Helper function that runs an aggregate command with the specified pipeline and the specified
 * command-level let vars.
 * @param {Object[]} pipeline
 * @param {Object} letVars
 */
function runPipelineWithLetCommandVars(pipeline, letVars) {
    return db.runCommand({aggregate: jsTestName(), pipeline, cursor: {}, let : letVars});
}

const pipelines = {
    single: [{$score: {score: "$single", normalization: "none"}}],
    double: [{$score: {score: "$double", normalization: "none"}}]
};

const pipelinesWithSigmoid = {
    single: [{$score: {score: "$single", normalization: "sigmoid"}}],
    double: [{$score: {score: "$double", normalization: "sigmoid"}}]
};

const pipelinesWithMinMaxScaler = {
    single: [{$score: {score: "$single", normalization: "minMaxScaler"}}],
    double: [{$score: {score: "$double", normalization: "minMaxScaler"}}]
};

const projectScore = {
    $project: {_id: 1, single: 1, double: 1, score: {$meta: "score"}}
};

const sortScore = {
    $sort: {score: -1, _id: 1}
};

const coll = db[jsTestName()];

/**
 * Helper function that populates the collection with documents containing two fields: single and
 * double. Field single has the range [1,10] and field double has the range [2,20]. Only called once
 * because all the test functions in this file use the same documents/collection.
 */
(function insertDocumentsWithSingleAndDoubleFields() {
    coll.drop();
    const bulk = coll.initializeUnorderedBulkOp();
    const nDocs = 10;

    for (let i = 1; i <= nDocs; i++) {
        bulk.insert({i, "single": i, "double": i * 2});
    }
    assert.commandWorked(bulk.execute());
})();

(function testScorePipelinesWithMinMaxScalerNormalization() {
    const pipelines = {
        single: [{$score: {score: "$single", normalization: "minMaxScaler"}}],
        double: [{$score: {score: "$double", normalization: "none"}}]
    };

    const actualResults =
        coll.aggregate([
                {
                    $scoreFusion:
                        {input: {pipelines, normalization: "none"}, combination: {method: "avg"}}
                },
                projectScore
            ])
            .toArray();

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResults =
        coll.aggregate([
                {
                    $setWindowFields: {
                        output: {
                            single_score: {
                                $minMaxScaler: {input: "$single"},
                            }
                        }
                    }
                },
                {
                    $project:
                        {_id: 1, single: 1, double: 1, score: {$avg: ["$single_score", "$double"]}}
                },
                sortScore
            ])
            .toArray();

    // Assert that every document returned by $scoreFusion is scored as expected using the
    // "avg" combination.method.
    assert.eq(actualResults, expectedResults);
})();

(function testScorePipelinesWithMinMaxScalerNormalizationAndScoreDetails() {
    const pipelines = {
        single: [{$score: {score: "$single", normalization: "minMaxScaler", scoreDetails: true}}],
        double: [{$score: {score: "$double", normalization: "none", scoreDetails: true}}]
    };

    const actualResults = coll.aggregate([
                                  {
                                      $scoreFusion: {
                                          input: {pipelines, normalization: "none"},
                                          combination: {method: "avg"},
                                          scoreDetails: true
                                      }
                                  },
                                  projectScore
                              ])
                              .toArray();

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResults =
        coll.aggregate([
                {
                    $setWindowFields: {
                        output: {
                            single_score: {
                                $minMaxScaler: {input: "$single"},
                            }
                        }
                    }
                },
                {
                    $project:
                        {_id: 1, single: 1, double: 1, score: {$avg: ["$single_score", "$double"]}}
                },
                sortScore
            ])
            .toArray();

    // Assert that every document returned by $scoreFusion is scored as expected using the
    // "avg" combination.method.
    assert.eq(actualResults, expectedResults);
})();

// Test Explanation: Neither of the document's score fields (single and double) will be normalized
// because the $score's normalization value is "none" and $scoreFusion's default normalization field
// is "none." Each document's score value will be averaged per the combination.method.

// The $scoreFusion pipeline sorts the documents in descending order by score (documents with
// the highest computed scores ranked first). Assert that the documents are in the correct order
// and have the correct values by asserting that the single, double, and score field's values match
// the expected values which are computed via adding the non-normalized input scores.

(function testCombinationMethodAvgOnMultiplePipelinesWithNoNormalization() {
    // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
    const actualResults =
        coll.aggregate([
                {
                    $scoreFusion:
                        {input: {pipelines, normalization: "none"}, combination: {method: "avg"}}
                },
                projectScore
            ])
            .toArray();

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResults =
        coll.aggregate([
                {$project: {_id: 1, single: 1, double: 1, score: {$avg: ["$single", "$double"]}}},
                sortScore
            ])
            .toArray();

    // Assert that every document returned by $scoreFusion is scored as expected using the
    // "avg" combination.method.
    assert.eq(actualResults, expectedResults);
})();

//-------------------------------------------------------------------------------------------------

// Test Explanation: Neither of the document's score fields (single and double) will be normalized
// because the $score's normalization value is "none" and $scoreFusion' s default normalization
// field is "none." Each document's score value will be averaged per the combination.method.

// The $scoreFusion pipeline sorts the documents in descending order by score (documents with
// the highest computed scores ranked first). Assert that the documents are in the correct order
// and have the correct values by asserting that the single, double, and score field's values
// match the expected values which are computed via averaging the non-normalized input scores.

(function testCombinationMethodAvgOnMultiplePipelinesWithNoNormalization() {
    // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
    const actualResults =
        coll.aggregate([
                {
                    $scoreFusion:
                        {input: {pipelines, normalization: "none"}, combination: {method: "avg"}}
                },
                projectScore
            ])
            .toArray();

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResults =
        coll.aggregate([
                {$project: {_id: 1, single: 1, double: 1, score: {$avg: ["$single", "$double"]}}},
                sortScore
            ])
            .toArray();

    // Assert that every document returned by $scoreFusion is scored as expected using the
    // "avg" combination.method.
    assert.eq(actualResults, expectedResults);
})();

//-------------------------------------------------------------------------------------------------

// Test Explanation: Run an aggregate command with a pipeline that specifies a
// $scoreFusion.combination.expression input that doesn't evaluate to a numeric value. This should
// error because the metadata's score value only accepts numeric values.

(function testCombinationExpressionOnMultiplePipelinesWithNoNormalizationNonNumericalExpression() {
    // Assert that a combination.expression that evaluates to a nonnumerical value cannot be stored
    // as metadata's score value (requires a numeric value) and throws a TypeMismatch error.
    assert.commandFailedWithCode(
        runPipeline([{
            $scoreFusion: {
                input: {pipelines, normalization: "none"},
                combination: {method: "expression", expression: {$toString: "2.5"}}
            }
        }]),
        ErrorCodes.TypeMismatch);
})();

//-------------------------------------------------------------------------------------------------

// Test Explanation: Neither of the document's score fields (single and double) will be normalized
// because the $score's normalization value is "none" and $scoreFusion's default normalization field
// is "none." Each document's score value will be added per the combination.expression.

// The $scoreFusion pipeline will be run as part of an aggregate command that also specified
// command-level let variables that overlap with the pipeline names specified as aggregate-level
// vars in combination.expression. The expected behavior is that the command-level vars will be
// shadowed and not available in the expression's scope.

// The $scoreFusion pipeline sorts the documents in descending order by score (documents with the
// highest computed scores ranked first). Assert that the documents are in the correct order and
// have the correct values by asserting that the single, double, and score field's values match the
// expected values which are computed via summing the non-normalized input scores.

(function testCombinationExpressionOnMultiplePipelinesWithNoNormalizationAndCommandLevelLetVars() {
    // Pipeline returns an array of documents, each with the score that $scoreFusion computed. The
    // aggregate's pipeline is run with command-level let variables that overlap with the pipeline
    // names ($$single and $$double) used in the custom combination.expression.
    const actualResults =
        runPipelineWithLetCommandVars(
            [
                {
                    $scoreFusion: {
                        input: {pipelines, normalization: "none"},
                        combination:
                            {method: "expression", expression: {$add: ["$$single", "$$double"]}}
                    }
                },
                projectScore
            ],
            {single: {$const: 5.0}, double: {$add: [1, 2, "$$single"]}})
            .cursor.firstBatch;

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResults =
        coll.aggregate([
                {$project: {_id: 1, single: 1, double: 1, score: {$add: ["$single", "$double"]}}},
                sortScore
            ])
            .toArray();

    // Assert that every document returned by $scoreFusion is scored as expected using the
    // "add" expression per combination.expression.
    assert.eq(actualResults, expectedResults);
})();

//-------------------------------------------------------------------------------------------------

// Test Explanation: Neither of the document's score fields (single and double) will be normalized
// because the $score's normalization value is "none" and $scoreFusion's default normalization field
// is "none." Each document's score value will be averaged per the combination.expression.

// The $scoreFusion pipeline sorts the documents in descending order by score (documents with
// the highest computed scores ranked first). Assert that the documents are in the correct order
// and have the correct values by asserting that the single, double, and score field's values match
// the expected values which are computed via averaging the non-normalized input scores.

(function testCombinationExpressionOnMultiplePipelinesWithNoNormalizationAndAvgExpression() {
    // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
    const actualResults =
        coll.aggregate([
                {
                    $scoreFusion: {
                        input: {pipelines, normalization: "none"},
                        combination:
                            {method: "expression", expression: {$avg: ["$$single", "$$double"]}}
                    }
                },
                projectScore
            ])
            .toArray();

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResults =
        coll.aggregate([
                {$project: {_id: 1, single: 1, double: 1, score: {$avg: ["$single", "$double"]}}},
                sortScore
            ])
            .toArray();

    // Assert that every document returned by $scoreFusion is scored as expected using the
    // "avg" combination.expression.
    assert.eq(actualResults, expectedResults);
})();

//-------------------------------------------------------------------------------------------------

// Test Explanation: Neither of the document's score fields (single and double) will be normalized
// because the $score's normalization value is "none" and $scoreFusion's default normalization field
// is "none." Each document's score value will be averaged per the combination.expression.

// Test 3 combination.expression values/cases: (1) Specify a valid root document field, (2) an
// invalid root document field, and (3) a string (invalid variable).

(function testCombinationExpressionOnMultiplePipelinesWithNoNormAndDocsVar() {
    // Pipeline returns an array of documents, each with the score that $scoreFusion computed. Note
    // that $docs.single is a valid root field specification because "single" is a field on the
    // original input document. The avg will be computed with respect to the following three fields:
    // "single", "double", and "single" (again) where the last field "single" is a collection field.
    const actualResults =
        coll.aggregate([
                {
                    $scoreFusion: {
                        input: {pipelines, normalization: "none"},
                        combination: {
                            method: "expression",
                            expression: {$avg: ["$$single", "$$double", "$docs.single"]}
                        }
                    }
                },
                projectScore
            ])
            .toArray();

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResults = coll.aggregate([
                                    {
                                        $project: {
                                            _id: 1,
                                            single: 1,
                                            double: 1,
                                            score: {$avg: ["$single", "$double", "$single"]}
                                        }
                                    },
                                    sortScore
                                ])
                                .toArray();

    // Assert that every document returned by $scoreFusion is scored as expected using the
    // "avg" combination.expression.
    assert.eq(actualResults, expectedResults);

    // Pipeline returns an array of documents, each with the score that $scoreFusion computed. Note
    // that the "$docs.dne" does not evaluate to anything because dne is not a field on the original
    // input document so the avg will be computed with respect to only the "$$single" and "$$double"
    // variables. Should ignore the invalid root document field/string and only compute the average
    // over the valid specified fields ($single and $double).
    const actualResultsWithUndefinedDocsField =
        coll.aggregate([
                {
                    $scoreFusion: {
                        input: {pipelines, normalization: "none"},
                        combination: {
                            method: "expression",
                            expression: {$avg: ["$$single", "$$double", "$docs.dne"]}
                        }
                    }
                },
                projectScore
            ])
            .toArray();

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResultsForUndefinedDocsField =
        coll.aggregate([
                {$project: {_id: 1, single: 1, double: 1, score: {$avg: ["$single", "$double"]}}},
                sortScore
            ])
            .toArray();

    // Assert that every document returned by $scoreFusion is scored as expected using the
    // "avg" combination.expression.
    assert.eq(actualResultsWithUndefinedDocsField, expectedResultsForUndefinedDocsField);

    // Pipeline returns an array of documents, each with the score that $scoreFusion computed. Note
    // that the "RANDOMGIBBERISH" string does not evaluate to anything and will not be factored into
    // the avg computation. Should ignore the invalid root document field/string and only compute
    // the average over the valid specified fields ($single and $double).
    const actualResultsWithPoorlyStatedVar =
        coll.aggregate([
                {
                    $scoreFusion: {
                        input: {pipelines, normalization: "none"},
                        combination: {
                            method: "expression",
                            expression: {$avg: ["$$single", "$$double", "RANDOMGIBBERISH"]}
                        }
                    }
                },
                projectScore
            ])
            .toArray();

    // Assert that every document returned by $scoreFusion is scored as expected using the
    // "avg" combination.expression.
    assert.eq(actualResultsWithPoorlyStatedVar, expectedResultsForUndefinedDocsField);
})();

//-------------------------------------------------------------------------------------------------

// Test Explanation: combination.expression specifies an undefined variable ('$$DOESNOTEXIST').
// Thus, the command should fail with the appropriate error code (17276 in this case).

(function testCombinationExpressionOnMultiplePipelinesWithNonexistentVariableInExpression() {
    // Assert that a combination.expression that contains an undefined variable cannot be stored
    // as metadata's score value and throws the correct error code.
    assert.commandFailedWithCode(
        runPipeline([{
            $scoreFusion: {
                input: {pipelines, normalization: "none"},
                combination: {
                    method: "expression",
                    expression: {$avg: ["$$single", "$$double", "$$DOESNOTEXIST"]}
                }
            }
        }]),
        17276);
})();

//-------------------------------------------------------------------------------------------------

// Test Explanation: Neither of the document's score fields (single and double) will be normalized
// because the $score's normalization value is "none" and $scoreFusion's default normalization field
// is "none." Each document's score value will be added per the combination.expression.

// The $scoreFusion pipeline will be run as part of an aggregate command that also specify a new
// command-level let variable, 'five' which has a value of 5.0. The expected behavior is that
// combination.expression will execute as expected and sum the following fields: '$docs.single',
// '$docs.double', and the variable '$$five'.

// The $scoreFusion pipeline sorts the documents in descending order by score (documents with the
// highest computed scores ranked first). Assert that the documents are in the correct order and
// have the correct values by asserting that the single, double, and score field's values match the
// expected values which are computed via summing the non-normalized input scores.

(function testCombinationExpressionOnMultiplePipelinesWithNoNormalizationAndCommandLevelLetVars() {
    // Pipeline returns an array of documents, each with the score that $scoreFusion computed. The
    // aggregate's pipeline is run with a command-level let variable that is referenced  in the
    // custom combination.expression.
    const actualResults =
        runPipelineWithLetCommandVars(
            [
                {
                    $scoreFusion: {
                        input: {pipelines, normalization: "none"},
                        combination: {
                            method: "expression",
                            expression: {$add: ["$$single", "$$double", "$$five"]}
                        }
                    }
                },
                projectScore
            ],
            {five: {$const: 5.0}})
            .cursor.firstBatch;

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResults =
        coll.aggregate([
                {
                    $project:
                        {_id: 1, single: 1, double: 1, score: {$add: ["$single", "$double", 5.0]}}
                },
                sortScore
            ])
            .toArray();

    // Assert that every document returned by $scoreFusion is scored as expected using the
    // "add" expression per combination.expression.
    assert.eq(actualResults, expectedResults);
})();

//-------------------------------------------------------------------------------------------------

// Test Explanation: Each document's score value will be averaged per the combination.method. Test 3
// different cases: (1) no normalization is applied, (2) each input pipeline has $sigmoid
// normalization applied, (3) each input pipeline has $sigmoid normalization applied and
// $scoreFusion applies $sigmoid normalization.

(function testCombinationAndCombinationWeightsOnMultiplePipelinesSigmoidNormalization() {
    const weights = {single: 0.5, double: 2};
    const combinationAvg = {
        weights: weights,
        method: "avg",
    };
    const combinationExpression = {
        method: "expression",
        expression: {$avg: ["$$single", "$$double"]}
    };
    const combinations = [combinationAvg, combinationExpression];
    function returnAverageWithWeights(weight1, weight2, numSigmoid) {
        if (numSigmoid === 1) {
            return [
                {$multiply: [{$sigmoid: "$single"}, weight1]},
                {$multiply: [{$sigmoid: "$double"}, weight2]}
            ];
        } else {
            return [
                {$multiply: [{$sigmoid: {$sigmoid: "$single"}}, weight1]},
                {$multiply: [{$sigmoid: {$sigmoid: "$double"}}, weight2]}
            ];
        }
    }
    for (let i = 0; i < combinations.length; i++) {
        let combination = combinations[i];
        // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
        let actualResults =
            coll.aggregate([
                    {
                        $scoreFusion: {
                            input: {pipelines: pipelinesWithSigmoid, normalization: "none"},
                            combination: combination
                        }
                    },
                    projectScore
                ])
                .toArray();
        // Pipeline returns an array of documents, each with the calculated expected score that
        // $scoreFusion should have computed.
        const avgWeight =
            (i === 0) ? returnAverageWithWeights(0.5, 2, 1) : returnAverageWithWeights(1, 1, 1);
        let expectedResults =
            coll.aggregate([
                    {$project: {_id: 1, single: 1, double: 1, score: {$avg: avgWeight}}},
                    sortScore
                ])
                .toArray();
        // Assert that every document returned by $scoreFusion is scored as expected using the
        // "avg" combination.method.
        assert.eq(actualResults, expectedResults);

        // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
        // NOTE: this pipeline uses input pipelines that set normalization to "sigmoid."
        actualResults =
            coll.aggregate([
                    {
                        $scoreFusion: {
                            input: {pipelines: pipelinesWithSigmoid, normalization: "none"},
                            combination: combination
                        }
                    },
                    projectScore
                ])
                .toArray();
        // Pipeline returns an array of documents, each with the calculated expected score that
        // $scoreFusion should have computed.
        expectedResults =
            coll.aggregate([
                    {$project: {_id: 1, single: 1, double: 1, score: {$avg: avgWeight}}},
                    sortScore
                ])
                .toArray();
        // Assert that every document returned by $scoreFusion is scored as expected using the
        // "avg" combination.method.
        assert.eq(actualResults, expectedResults);

        // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
        // NOTE: this pipeline uses input pipelines that set normalization to "sigmoid" and sets
        // $scoreFusion's normalization to "sigmoid."
        actualResults =
            coll.aggregate([
                    {
                        $scoreFusion: {
                            input: {pipelines: pipelinesWithSigmoid, normalization: "sigmoid"},
                            combination: combination
                        }
                    },
                    projectScore
                ])
                .toArray();
        // Pipeline returns an array of documents, each with the calculated expected score that
        // $scoreFusion should have computed.
        const avgWeightDouble =
            (i === 0) ? returnAverageWithWeights(0.5, 2, 2) : returnAverageWithWeights(1, 1, 2);
        expectedResults =
            coll.aggregate([
                    {$project: {_id: 1, single: 1, double: 1, score: {$avg: avgWeightDouble}}},
                    sortScore
                ])
                .toArray();
        // Assert that every document returned by $scoreFusion is scored as expected using the
        // "avg" combination.method.
        assert.eq(actualResults, expectedResults);
    }
})();

//-------------------------------------------------------------------------------------------------

// Test Explanation: Each document's score value will be averaged per the combination.method. Test 2
// different cases: (1) each input pipeline has $minMaxScaler normalization applied, (2) each input
// pipeline has $minMaxScaler normalization applied and $scoreFusion applies $minMaxScaler
// normalization.

(function testCombinationAndCombinationWeightsOnMultiplePipelinesMinMaxScalerNormalization() {
    const weights = {single: 0.5, double: 2};
    const combinationAvg = {
        weights: weights,
        method: "avg",
    };
    const combinationExpression = {
        method: "expression",
        expression: {$avg: ["$$single", "$$double"]}
    };
    const combinations = [combinationAvg, combinationExpression];
    function returnAverageWithWeights(weight1, weight2, inludeMultiply = false) {
        if (inludeMultiply) {
            return [
                {$multiply: ["$single_score", weight1]},
                {$multiply: ["$double_score", weight2]}
            ];
        } else {
            return [["$single_score", weight1], ["$double_score", weight2]];
        }
    }
    for (let i = 0; i < combinations.length; i++) {
        let combination = combinations[i];
        // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
        // NOTE:
        // this pipeline uses input pipelines that set normalization to "minMaxScaler."
        let actualResults =
            coll.aggregate([
                    {
                        $scoreFusion: {
                            input: {pipelines: pipelinesWithMinMaxScaler, normalization: "none"},
                            combination: combination,
                        }
                    },
                    projectScore
                ])
                .toArray();
        // Pipeline returns an array of documents, each with the calculated expected score that
        // $scoreFusion should have computed.
        const avgWeight = (i === 0) ? returnAverageWithWeights(0.5, 2, true)
                                    : returnAverageWithWeights(1, 1, true);
        let expectedResults =
            coll.aggregate([
                    {
                        $setWindowFields: {
                            output: {
                                single_score: {
                                    $minMaxScaler: {input: "$single"},
                                    window: {documents: ["unbounded", "unbounded"]}
                                },
                                double_score: {
                                    $minMaxScaler: {input: "$double"},
                                    window: {documents: ["unbounded", "unbounded"]}
                                }
                            }
                        }
                    },
                    {$project: {_id: 1, single: 1, double: 1, score: {$avg: avgWeight}}},
                    sortScore
                ])
                .toArray();
        // Assert that every document returned by $scoreFusion is scored as expected using the
        // "avg" combination.method.
        assert.eq(actualResults, expectedResults);
        // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
        // NOTE:
        // this pipeline uses input pipelines that set normalization to "minMaxScaler."
        actualResults =
            coll.aggregate([
                    {
                        $scoreFusion: {
                            input: {pipelines: pipelinesWithMinMaxScaler, normalization: "none"},
                            combination: combination
                        }
                    },
                    projectScore
                ])
                .toArray();
        // Pipeline returns an array of documents, each with the calculated expected score that
        // $scoreFusion should have computed.
        expectedResults =
            coll.aggregate([
                    {
                        $setWindowFields: {
                            output: {
                                single_score: {
                                    $minMaxScaler: {input: "$single"},
                                    window: {documents: ["unbounded", "unbounded"]}
                                },
                                double_score: {
                                    $minMaxScaler: {input: "$double"},
                                    window: {documents: ["unbounded", "unbounded"]}
                                }
                            }
                        }
                    },
                    {$project: {_id: 1, single: 1, double: 1, score: {$avg: avgWeight}}},
                    sortScore
                ])
                .toArray();
        // Assert that every document returned by $scoreFusion is scored as expected using the
        // "avg" combination.method.
        assert.eq(actualResults, expectedResults);

        // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
        // NOTE: this pipeline uses input pipelines that set normalization to "minMaxScaler" and
        // sets $scoreFusion's normalization to "minMaxScaler."
        actualResults = coll.aggregate([
                                {
                                    $scoreFusion: {
                                        input: {
                                            pipelines: pipelinesWithMinMaxScaler,
                                            normalization: "minMaxScaler"
                                        },
                                        combination: combination
                                    }
                                },
                                projectScore
                            ])
                            .toArray();
        // Pipeline returns an array of documents, each with the calculated expected score that
        // $scoreFusion should have computed.
        const avgWeights =
            (i === 0) ? returnAverageWithWeights(0.5, 2) : returnAverageWithWeights(1, 1);
        expectedResults =
            coll.aggregate([
                    {
                        $setWindowFields: {
                            output: {
                                single_score: {
                                    $minMaxScaler: {input: "$single"},
                                    window: {documents: ["unbounded", "unbounded"]}
                                },
                                double_score: {
                                    $minMaxScaler: {input: "$double"},
                                    window: {documents: ["unbounded", "unbounded"]}
                                }
                            }
                        }
                    },
                    {
                        $setWindowFields: {
                            output: {
                                single_score_minMaxScaler: {
                                    $minMaxScaler: {input: {$multiply: avgWeights[0]}},
                                    window: {documents: ["unbounded", "unbounded"]}
                                },
                                double_score_minMaxScaler: {
                                    $minMaxScaler: {input: {$multiply: avgWeights[1]}},
                                    window: {documents: ["unbounded", "unbounded"]}
                                }
                            }
                        }
                    },
                    {
                        $project: {
                            _id: 1,
                            single: 1,
                            double: 1,
                            score: {
                                $avg: [
                                    {$multiply: ["$single_score_minMaxScaler"]},
                                    {$multiply: ["$double_score_minMaxScaler"]}
                                ]
                            }
                        }
                    },
                    sortScore
                ])
                .toArray();
        // Assert that every document returned by $scoreFusion is scored as expected using the
        // "avg" combination.method.
        assert.eq(actualResults, expectedResults);
    }
})();
