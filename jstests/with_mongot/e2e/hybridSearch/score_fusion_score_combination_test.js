/**
 * Tests that the $scoreFusion.combination.method works as expected.
 * @tags: [ featureFlagRankFusionFull, featureFlagSearchHybridScoringFull, requires_fcv_81 ]
 */

const coll = db[jsTestName()];

// Test Explanation: Add two fields to each document called single and double. Field single
// has the range [1,10] and field double has the range [2,20].

// Neither of the document's score fields (single and double) will be normalized because the
// $score's normalization value is "none" and $scoreFusion's default normalization field is "none."
// Each document's score values will be summed per the combination.method.

// The $scoreFusion pipeline sorts the documents in descending order by score (documents with
// the highest computed scores ranked first). Assert that the documents are in the correct order
// and have the correct values by asserting that the single, double, and score field's values match
// the expected values which are computed via adding the non-normalized input scores.

(function testCombinationMethodSumOnMultiplePipelinesWithNoNormalization() {
    coll.drop();
    const bulk = coll.initializeUnorderedBulkOp();
    const nDocs = 10;

    for (let i = 1; i <= nDocs; i++) {
        bulk.insert({i, "single": i, "double": i * 2});
    }
    assert.commandWorked(bulk.execute());

    // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
    const actualResults =
        coll.aggregate([
                {
                    $scoreFusion: {
                        input: {
                            pipelines: {
                                single: [{$score: {score: "$single", normalizeFunction: "none"}}],
                                double: [{$score: {score: "$double", normalizeFunction: "none"}}]
                            },
                            normalization: "none"
                        },
                        combination: {method: "sum"}
                    }
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

// Test Explanation: Add two fields to each document called single and double. Field single
// has the range [1,10] and field double has the range [2,20].

// Neither of the document's score fields (single and double) will be normalized because the
// $score's normalization value is "none" and $scoreFusion's default normalization field is "none."
// Each document's score values will be averaged per the combination.method.

// The $scoreFusion pipeline sorts the documents in descending order by score (documents with
// the highest computed scores ranked first). Assert that the documents are in the correct order
// and have the correct values by asserting that the single, double, and score field's values match
// the expected values which are computed via averaging the non-normalized input scores.

(function testCombinationMethodAvgOnMultiplePipelinesWithNoNormalization() {
    coll.drop();
    const bulk = coll.initializeUnorderedBulkOp();
    const nDocs = 10;

    for (let i = 1; i <= nDocs; i++) {
        bulk.insert({i, "single": i, "double": i * 2});
    }
    assert.commandWorked(bulk.execute());

    // Pipeline returns an array of documents, each with the score that $scoreFusion computed.
    const actualResults =
        coll.aggregate([
                {
                    $scoreFusion: {
                        input: {
                            pipelines: {
                                single: [{$score: {score: "$single", normalizeFunction: "none"}}],
                                double: [{$score: {score: "$double", normalizeFunction: "none"}}]
                            },
                            normalization: "none"
                        },
                        combination: {method: "avg"}
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
    // "avg" combination.method.
    assert.eq(actualResults, expectedResults);
})();
