/**
 * Tests that the $sigmoid normalization option on $scoreFusion computes the correct values.
 * @tags: [ featureFlagRankFusionFull, featureFlagSearchHybridScoringFull, requires_fcv_81 ]
 */

/**
 * Helper function that checks whether a given score is between the range [min,max].
 * @param {Number} score
 * @param {Number} min
 * @param {Number} max
 */
function scoreInRange(score, min, max) {
    return (score >= min && score <= max);
}

const coll = db[jsTestName()];

// General Note: For all of these tests, we rely on the $sigmoid operator and its underlying logic.
// These tests will pass even if $sigmoid's underlying logic/formula changes.

// Test Explanation: Add a field to each document called negative_score with the range [-1,-10].

// Each document's negative_score field will undergo sigmoid normalization when the $score stage is
// executed because the default normalization for $score is sigmoid. This normalized value, bounded
// between [0,1], will be saved on $meta: score. Then each normalized score metadata value will
// undergo sigmoid normalization again for the $scoreFusion stage.

// The $scoreFusion pipeline sorts the documents in descending order by score (documents with
// the highest computed scores ranked first). Assert that the documents are in the correct order and
// have the correct values by asserting that the negative_score and score field's values match the
// expected values which are computed via taking the sigmoid of the sigmoided input score. Also
// verify that the scores were normalized to the correct range and bounded between [0,1].

(function testSigmoidNormalizationOnSinglePipelineWithNegInputScoresAndScoreSigmoidNormalization() {
    coll.drop();
    const bulk = coll.initializeUnorderedBulkOp();
    const nDocs = 10;
    for (let i = 1; i <= nDocs; i++) {
        bulk.insert({"negative_score": -1 * i});
    }
    assert.commandWorked(bulk.execute());

    // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
    const actualResults =
        coll.aggregate([
                {
                    $scoreFusion: {
                        input: {
                            pipelines: {
                                negativeScore:
                                    [{$score: {score: "$negative_score", normalization: "sigmoid"}}]
                            },
                            normalization: "sigmoid"
                        }
                    }
                },
                {$project: {_id: 0, negative_score: 1, score: {$meta: "score"}}}
            ])
            .toArray();

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResults =
        coll.aggregate([
                {
                    $project: {
                        _id: 1,
                        negative_score: 1,
                        score: {$avg: [{$sigmoid: {$sigmoid: "$negative_score"}}]}
                    }
                },
                {$sort: {score: -1, _id: 1}},
                {$project: {_id: 0}}
            ])
            .toArray();

    // Assert that every document returned by $scoreFusion is scored as expected using the
    // $sigmoid
    // normalization.
    assert.eq(actualResults, expectedResults);

    // Assert that each document's normalized score value is bounded between [0,1].
    for (let i = 0; i < actualResults.length; i++) {
        const doc = actualResults[i];
        assert.eq(scoreInRange(doc.score, 0, 1), true);
    }
})();

//-------------------------------------------------------------------------------------------------

// Test Explanation: Add a field to each document called score_val with the range [1,100].

// Each document's score_val field will be saved on $meta: score when the $score stage is executed
// (note that $score's specified normalization value below is "none"). Then each score metadata
// value will undergo sigmoid normalization for the $scoreFusion stage.

// The $scoreFusion pipeline sorts the documents in descending order by score (documents with
// the highest computed scores ranked first). Assert that the documents are in the correct order and
// have the correct values by asserting that the score_val and score field's values match the
// expected values which are computed via taking the sigmoid of the input score. Also verify that
// the scores were normalized to the correct range and bounded between [0,1].

(function testSigmoidNormalizationOnSingleInputPipelineWithNoScoreSigmoidNormalization() {
    coll.drop();
    const bulk = coll.initializeUnorderedBulkOp();
    const nDocs = 100;
    for (let i = 1; i <= nDocs; i++) {
        bulk.insert({"score_val": i});
    }
    assert.commandWorked(bulk.execute());

    // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
    const actualResults =
        coll.aggregate([
                {
                    $scoreFusion: {
                        input: {
                            pipelines: {
                                scoreVal: [{$score: {score: "$score_val", normalization: "none"}}]
                            },
                            normalization: "sigmoid"
                        }
                    }
                },
                {$project: {_id: 0, score_val: 1, score: {$meta: "score"}}}
            ])
            .toArray();

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResults =
        coll.aggregate([
                {$project: {_id: 1, score_val: 1, score: {$avg: [{$sigmoid: "$score_val"}]}}},
                {$sort: {score: -1, _id: 1}},
                {$project: {_id: 0}}
            ])
            .toArray();

    // Assert that every document returned by $scoreFusion is scored as expected using the $sigmoid
    // normalization.
    assert.eq(actualResults, expectedResults);

    // Assert that each document's normalized score value is bounded between [0,1].
    for (let i = 0; i < actualResults.length; i++) {
        const doc = actualResults[i];
        assert.eq(scoreInRange(doc.score, 0, 1), true);
    }
})();

//-------------------------------------------------------------------------------------------------

// Test Explanation: Add fields to each document called single and double with the ranges [1,100]
// and [2,200] respectively.

// Each document's single and double fields will be saved on $meta: score when the $score stage is
// executed (note that $score's specified normalization value below is "none"). Then each score
// metadata value will undergo sigmoid normalization for the $scoreFusion stage.

// The $scoreFusion pipeline sorts the documents in descending order by score (documents with
// the highest computed scores ranked first). Assert that the documents are in the correct order and
// have the correct values by asserting that the single, double, and score field's values match the
// expected values which are computed via adding the sigmoids of the input scores. Also verify that
// the scores were normalized to the correct range and bounded between [0,2].

(function testSigmoidNormalizationOnMultipleInputPipelinesWithNoScoreSigmoidNormalization() {
    coll.drop();
    const bulk = coll.initializeUnorderedBulkOp();
    const nDocs = 100;
    for (let i = 1; i <= nDocs; i++) {
        bulk.insert({"single": i, "double": i * 2});
    }
    assert.commandWorked(bulk.execute());

    // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
    const actualResults =
        coll.aggregate([
                {
                    $scoreFusion: {
                        input: {
                            pipelines: {
                                single: [{$score: {score: "$single", normalization: "none"}}],
                                double: [{$score: {score: "$double", normalization: "none"}}]
                            },
                            normalization: "sigmoid"
                        }
                    }
                },
                {$project: {_id: 1, single: 1, double: 1, score: {$meta: "score"}}}
            ])
            .toArray();

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResults =
        coll.aggregate([
                {
                    $project: {
                        _id: 1,
                        single: 1,
                        double: 1,
                        score: {$avg: [{$sigmoid: "$single"}, {$sigmoid: "$double"}]}
                    }
                },
                {$sort: {score: -1, _id: 1}}
            ])
            .toArray();

    // Assert that every document returned by $scoreFusion is scored as expected using the $sigmoid
    // normalization.
    assert.eq(actualResults, expectedResults);

    // Assert that each document's normalized score value is bounded between [0,2].
    for (let i = 0; i < actualResults.length; i++) {
        const doc = actualResults[i];
        assert.eq(scoreInRange(doc.score, 0, 2), true);
    }
})();

//-------------------------------------------------------------------------------------------------

// Test Explanation: Add two fields to each document called score_50 and score_10. Field score_50
// has the range [0,50] and field score_10 has the range [0,10].

// Each document's score field (score_50 and score_10) will undergo sigmoid normalization when the
// $score stage is executed because the default normalization for $score is sigmoid. This normalized
// value, bounded between [0,1], will be saved on $meta: score. Then each normalized score metadata
// value will undergo sigmoid normalization again for the $scoreFusion stage.

// The $scoreFusion pipeline sorts the documents in descending order by score (documents with
// the highest computed scores ranked first). Assert that the documents are in the correct order and
// have the correct values by asserting that the score_50, score_10, and score field's values match
// the expected values which are computed via adding the input scores which have undergone $sigmoid
// twice. Also verify that the scores were normalized to the correct range and bounded between
// [0,2].

(function testSigmoidNormalizationOnMultiplePipelinesWithScoreSigmoidNormalization() {
    coll.drop();
    const bulk = coll.initializeUnorderedBulkOp();

    const nDocs = 50;
    for (let i = 0; i <= nDocs; i++) {
        if (i % 5 === 0) {
            bulk.insert({"score_50": i, "score_10": i / 5});
        } else {
            // Have to specify score_10 or $score will fail with: Plan Aggregator found null
            // instead of numeric value
            bulk.insert({"score_50": i, "score_10": 0});
        }
    }
    assert.commandWorked(bulk.execute());

    // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
    const actualResults =
        coll.aggregate([
                {
                    $scoreFusion: {
                        input: {
                            pipelines: {
                                score50: [{$score: {score: "$score_50", normalization: "sigmoid"}}],
                                score10: [{$score: {score: "$score_10", normalization: "sigmoid"}}]
                            },
                            normalization: "sigmoid"
                        }
                    }
                },
                {$project: {_id: 0, score_10: 1, score_50: 1, score: {$meta: "score"}}}
            ])
            .toArray();

    // Pipeline returns an array of documents, each with the calculated expected score that
    // $scoreFusion should have computed.
    const expectedResults = coll.aggregate([
                                    {
                                        $project: {
                                            _id: 1,
                                            score_10: 1,
                                            score_50: 1,
                                            score: {
                                                $avg: [
                                                    {$sigmoid: {$sigmoid: "$score_10"}},
                                                    {$sigmoid: {$sigmoid: "$score_50"}}
                                                ]
                                            }
                                        }
                                    },
                                    {$sort: {score: -1, _id: 1}},
                                    {$project: {_id: 0}}
                                ])
                                .toArray();

    // Assert that every document returned by $scoreFusion is scored as expected using the $sigmoid
    // normalization.
    assert.eq(actualResults, expectedResults);

    // Assert that each document's normalized score value is bounded between [0,2].
    for (let i = 0; i < actualResults.length; i++) {
        const doc = actualResults[i];
        if (doc.score_10 === 0) {
            // When score_10 is 0 (which it is for ~80% of the documents), then its normalized
            // value can be no more than 0.5 so the range for the final score is [0.5,1.5].
            assert.eq(scoreInRange(doc.score, 0.5, 1.5), true);
        } else {
            assert.eq(scoreInRange(doc.score, 0, 2), true);
        }
    }
})();
