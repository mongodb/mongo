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
    single: [{$score: {score: "$single", normalizeFunction: "none"}}],
    double: [{$score: {score: "$double", normalizeFunction: "none"}}]
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

// Test Explanation: Neither of the document's score fields (single and double) will be normalized
// because the $score's normalization value is "none" and $scoreFusion's default normalization field
// is "none." Each document's score value will be summed per the combination.method.

// The $scoreFusion pipeline sorts the documents in descending order by score (documents with
// the highest computed scores ranked first). Assert that the documents are in the correct order
// and have the correct values by asserting that the single, double, and score field's values match
// the expected values which are computed via adding the non-normalized input scores.

(function testCombinationMethodSumOnMultiplePipelinesWithNoNormalization() {
    // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
    const actualResults =
        coll.aggregate([
                {
                    $scoreFusion:
                        {input: {pipelines, normalization: "none"}, combination: {method: "sum"}}
                },
                {$project: {_id: 1, single: 1, double: 1, score: {$meta: "score"}}}
            ])
            .toArray();

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResults =
        coll.aggregate([
                {$project: {_id: 1, single: 1, double: 1, score: {$add: ["$single", "$double"]}}},
                {$sort: {score: -1, _id: 1}}
            ])
            .toArray();

    // Assert that every document returned by $scoreFusion is scored as expected using the
    // "sum" combination.method.
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
                {$project: {_id: 1, single: 1, double: 1, score: {$meta: "score"}}}
            ])
            .toArray();

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResults =
        coll.aggregate([
                {$project: {_id: 1, single: 1, double: 1, score: {$avg: ["$single", "$double"]}}},
                {$sort: {score: -1, _id: 1}}
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
                {$project: {_id: 1, single: 1, double: 1, score: {$meta: "score"}}}
            ],
            {single: {$const: 5.0}, double: {$add: [1, 2, "$$single"]}})
            .cursor.firstBatch;

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResults =
        coll.aggregate([
                {$project: {_id: 1, single: 1, double: 1, score: {$add: ["$single", "$double"]}}},
                {$sort: {score: -1, _id: 1}}
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
                {$project: {_id: 1, single: 1, double: 1, score: {$meta: "score"}}}
            ])
            .toArray();

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResults =
        coll.aggregate([
                {$project: {_id: 1, single: 1, double: 1, score: {$avg: ["$single", "$double"]}}},
                {$sort: {score: -1, _id: 1}}
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
                {$project: {_id: 1, single: 1, double: 1, score: {$meta: "score"}}}
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
                                    {$sort: {score: -1, _id: 1}}
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
                {$project: {_id: 1, single: 1, double: 1, score: {$meta: "score"}}}
            ])
            .toArray();

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResultsForUndefinedDocsField =
        coll.aggregate([
                {$project: {_id: 1, single: 1, double: 1, score: {$avg: ["$single", "$double"]}}},
                {$sort: {score: -1, _id: 1}}
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
                {$project: {_id: 1, single: 1, double: 1, score: {$meta: "score"}}}
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
                {$project: {_id: 1, single: 1, double: 1, score: {$meta: "score"}}}
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
                {$sort: {score: -1, _id: 1}}
            ])
            .toArray();

    // Assert that every document returned by $scoreFusion is scored as expected using the
    // "add" expression per combination.expression.
    assert.eq(actualResults, expectedResults);
})();
