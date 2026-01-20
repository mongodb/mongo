/**
 * Tests the vector similarity family of expressions in aggregation pipelines.
 *
 * @tags: [
 *   featureFlagVectorSimilarityExpressions,
 *   requires_fcv_83,
 * ]
 */

const coll = db[jsTestName()];

{
    const similarityOperators = [
        {
            operator: "$similarityCosine",
            expectedRawKey: "expectedCosine",
            expectedNormalizedKey: "expectedNormalizedCosine",
        },
        {
            operator: "$similarityDotProduct",
            expectedRawKey: "expectedDotProduct",
            expectedNormalizedKey: "expectedNormalizedDotProduct",
        },
        {
            operator: "$similarityEuclidean",
            expectedRawKey: "expectedEuclidean",
            expectedNormalizedKey: "expectedNormalizedEuclidean",
        },
    ];

    const testCases = [
        {
            document: {
                left: [NumberInt(1), NumberInt(2)],
                right: [NumberInt(3), NumberInt(4)],
                leftVector: BinData(9, "AwABAg=="),
                rightVector: BinData(9, "AwADBA=="),
            },
            expectedCosine: 0.98387,
            expectedNormalizedCosine: 0.991935,
            expectedDotProduct: 11,
            expectedNormalizedDotProduct: 6,
            expectedEuclidean: 2.828427,
            expectedNormalizedEuclidean: 0.2612,
        },
        {
            document: {
                left: [NumberInt(1), 2.5],
                right: [NumberInt(3), NumberInt(5)],
                leftVector: BinData(9, "JwAAAIA/AAAgQA=="),
                rightVector: BinData(9, "AwADBQ=="),
            },
            expectedCosine: 0.98724,
            expectedNormalizedCosine: 0.99362,
            expectedDotProduct: 15.5,
            expectedNormalizedDotProduct: 8.25,
            expectedEuclidean: 3.20156,
            expectedNormalizedEuclidean: 0.23801,
        },
        {
            document: {
                left: [NumberInt(1)],
                right: [NumberInt(2)],
                leftVector: BinData(9, "EAeA"),
                rightVector: BinData(9, "AwAC"),
            },
            expectedCosine: 1,
            expectedNormalizedCosine: 1,
            expectedDotProduct: 2,
            expectedNormalizedDotProduct: 1.5,
            expectedEuclidean: 1,
            expectedNormalizedEuclidean: 0.5,
        },
        {
            document: {
                left: [NumberInt(2), NumberInt(3)],
                right: [NumberInt(4), NumberInt(5)],
                leftVector: BinData(9, "AwACAw=="),
                rightVector: BinData(9, "AwAEBQ=="),
            },
            expectedCosine: 0.99624,
            expectedNormalizedCosine: 0.99812,
            expectedDotProduct: 23,
            expectedNormalizedDotProduct: 12,
            expectedEuclidean: 2.82843,
            expectedNormalizedEuclidean: 0.2612,
        },
        {
            document: {
                left: [NumberInt(1), NumberInt(2), NumberInt(3)],
                right: [NumberInt(4), NumberInt(5), NumberInt(6)],
                leftVector: BinData(9, "AwABAgM="),
                rightVector: BinData(9, "AwAEBQY="),
            },
            expectedCosine: 0.974632,
            expectedNormalizedCosine: 0.987316,
            expectedDotProduct: 32,
            expectedNormalizedDotProduct: 16.5,
            expectedEuclidean: 5.19615,
            expectedNormalizedEuclidean: 0.16139,
        },
        {
            document: {
                left: [],
                right: [],
                leftVector: BinData(9, "EAA="),
                rightVector: BinData(9, "EAA="),
            },
            expectedCosine: 0,
            expectedNormalizedCosine: 0.5,
            expectedDotProduct: 0,
            expectedNormalizedDotProduct: 0.5,
            expectedEuclidean: 0,
            expectedNormalizedEuclidean: 1,
        },
    ];

    testCases.forEach(function (testCase) {
        assert.commandWorked(coll.insertOne(testCase.document));

        similarityOperators.forEach(function ({operator, expectedRawKey, expectedNormalizedKey}) {
            // Test with standard numeric array input.
            const result = coll
                .aggregate([
                    {
                        $project: {
                            computedRaw: {[operator]: ["$left", "$right"]},
                            computedNormalized: {[operator]: {vectors: ["$left", "$right"], score: true}},
                        },
                    },
                ])
                .toArray();

            assert.eq(result.length, 1, `${operator} result should return one document`);
            assert.close(result[0].computedRaw, testCase[expectedRawKey], `${operator} raw score mismatch`);
            assert.close(
                result[0].computedNormalized,
                testCase[expectedNormalizedKey],
                `${operator} normalized score mismatch`,
            );

            // Test with binData vector input.
            const resultBinData = coll
                .aggregate([
                    {
                        $project: {
                            computedRaw: {[operator]: ["$leftVector", "$rightVector"]},
                            computedNormalized: {
                                [operator]: {
                                    vectors: ["$leftVector", "$rightVector"],
                                    score: true,
                                },
                            },
                        },
                    },
                ])
                .toArray();

            assert.eq(resultBinData.length, 1, `${operator} binData result should return one document`);
            assert.close(
                resultBinData[0].computedRaw,
                testCase[expectedRawKey],
                `${operator} binData raw score mismatch`,
            );
            assert.close(
                resultBinData[0].computedNormalized,
                testCase[expectedNormalizedKey],
                `${operator} binData normalized score mismatch`,
            );
        });

        assert(coll.drop());
    });
}

{
    // Test error codes on incorrect use of similarity operators.
    const errorTestCases = [
        // Type mismatch - non-array inputs.
        {document: {left: 1, right: [1, 2]}, errorCode: 10413200},
        {document: {left: [1, 2], right: "string"}, errorCode: 10413200},
        {document: {left: {object: true}, right: [1, 2]}, errorCode: 10413200},

        // Arrays of different lengths.
        {document: {left: [1, 2, 3], right: [4, 5]}, errorCode: 10413202},

        // Arrays containing non-numeric elements.
        {document: {left: [1, "string"], right: [3, 4]}, errorCode: 10413203},
        {document: {left: [1, 2], right: [3, null]}, errorCode: 10413204},
    ];

    errorTestCases.forEach(function (testCase) {
        assert.commandWorked(coll.insert(testCase.document));

        for (const expr of ["$similarityCosine", "$similarityDotProduct", "$similarityEuclidean"]) {
            assert.throwsWithCode(
                () => coll.aggregate({$project: {computed: {[expr]: ["$left", "$right"]}}}),
                testCase.errorCode,
            );
        }

        assert(coll.drop());
    });
}
