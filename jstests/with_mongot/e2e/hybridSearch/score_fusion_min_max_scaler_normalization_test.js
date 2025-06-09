/**
 * Tests that the $minMaxScaler normalization option on $scoreFusion computes the correct values.
 * @tags: [ featureFlagSearchHybridScoringFull, requires_fcv_81 ]
 */

const coll = db[jsTestName()];

function scoreInRange(score, min, max) {
    return (score >= min && score <= max);
}

/**
 * General Note: For all of these tests, we rely on the $minMaxScaler operator and its underlying
 * logic. These tests will pass even if $minMaxScaler's underlying logic/formula changes.
 */

/**
 * Test Explanation: Add a field to each document called big_score with the range [1,10].
 *
 * Each document's big_score field will be saved on $meta: score. Then each normalized score
 * metadata value will undergo $minMaxScaler normalization for the $scoreFusion stage.
 *
 * The $scoreFusion pipeline sorts the documents in descending order by score (documents with the
 * highest computed scores ranked first). Assert that the documents are in the correct order and
 * have the correct values by asserting that the big_score and score field's values match the
 * expected values which are computed via taking the minMaxScaler of the input score. Also verify
 * that the scores were normalized to the correct range and bounded between [0,1].
 */
(function testMinMaxScalerNormalizationOnSingleInputPipeline() {
    coll.drop();
    const bulk = coll.initializeUnorderedBulkOp();
    const nDocs = 10;
    for (let i = 1; i <= nDocs; i++) {
        bulk.insert({"big_score": i});
    }
    assert.commandWorked(bulk.execute());

    // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
    const actualResults =
        coll.aggregate([
                {
                    $scoreFusion: {
                        input: {
                            pipelines: {
                                bigScore: [{$score: {score: "$big_score", normalization: "none"}}]
                            },
                            normalization: "minMaxScaler"
                        }
                    }
                },
                {$project: {_id: 0, big_score: 1, score: {$meta: "score"}}}
            ])
            .toArray();
    assert.eq(nDocs, actualResults.length);

    const expectedResults =
        coll.aggregate([
                {
                    $setWindowFields: {
                        output: {
                            score: {
                                $minMaxScaler: {input: "$big_score"},
                                window: {documents: ["unbounded", "unbounded"]}
                            }
                        }
                    }
                },
                {$sort: {score: -1, _id: 1}},
                {$project: {_id: 0, big_score: 1, score: 1}}
            ])
            .toArray();
    assert.eq(actualResults, expectedResults);

    for (let i = 0; i < nDocs; i++) {
        const actualResult = actualResults[i];
        assert.eq(scoreInRange(actualResult.score, 0, 1), true);
    }
});

/**
 * Test Explanation: Add fields to each document called single and double with the ranges [1, 100]
 * and [2, 200] respectively.
 *
 * Each document's single and double fields will be saved on $meta: score when the $score stage is
 * executed.  Then each score metadata value will undergo minMaxScaler normalization for the
 * $scoreFusion stage.
 *
 * The $scoreFusion pipeline sorts the documents in descending order by score
 * (documents with the highest computed scores ranked first). Assert that the documents are in the
 * correct order and correct values by asserting that the single, double, and score field's values
 * match the expected values which are computed via adding the minMaxScaler normalizations of the
 * input scores.  Also verify that the scores were normalized to the correct range and bounded
 * between [0, 1].
 */
(function testMinMaxScalerNormalizationOnMultipleInputPipeline() {
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
                            normalization: "minMaxScaler"
                        }
                    }
                },
                {$project: {_id: 1, single: 1, double: 1, score: {$meta: "score"}}}
            ])
            .toArray();
    assert.eq(nDocs, actualResults.length);

    const expectedResults =
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
                    $project: {
                        _id: 1,
                        single: 1,
                        double: 1,
                        score: {$avg: ["$single_score", "$double_score"]}
                    }
                },
                {$sort: {score: -1, _id: 1}}
            ])
            .toArray();
    assert.eq(actualResults, expectedResults);

    for (let i = 0; i < nDocs; i++) {
        const actualResult = actualResults[i];
        assert.eq(scoreInRange(actualResult.score, 0, 2), true);
    }
})();
