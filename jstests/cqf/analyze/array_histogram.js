/**
 * This test verifies array histograms are both generated and estimated correctly.
 */
(function() {
"use strict";

load('jstests/libs/ce_stats_utils.js');

runHistogramsTest(function verifyArrayHistograms() {
    const coll = db.array_histogram;
    coll.drop();

    const docs = [
        {_id: 0, a: "scalar"},
        {_id: 1, a: "mixed"},
        {_id: 2, a: ["array"]},
        {_id: 3, a: ["array", "array"]},
        {_id: 4, a: ["array", "mixed"]},
        {_id: 5, a: []}
    ];

    const idx = {a: 1};
    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(coll.createIndex(idx));

    const expectedHistogram = {
        _id: "a",
        statistics: {
            typeCount: [
                {typeName: "StringSmall", count: 2},
                {typeName: "Array", count: 4},
            ],
            scalarHistogram: {
                buckets: [
                    {
                        boundaryCount: 1,
                        rangeCount: 0,
                        cumulativeCount: 1,
                        rangeDistincts: 0,
                        cumulativeDistincts: 1
                    },
                    {
                        boundaryCount: 1,
                        rangeCount: 0,
                        cumulativeCount: 2,
                        rangeDistincts: 0,
                        cumulativeDistincts: 2
                    }
                ],
                bounds: ["mixed", "scalar"]
            },
            arrayStatistics: {
                uniqueHistogram: {
                    buckets: [
                        {
                            boundaryCount: 3,
                            rangeCount: 0,
                            cumulativeCount: 3,
                            rangeDistincts: 0,
                            cumulativeDistincts: 1
                        },
                        {
                            boundaryCount: 1,
                            rangeCount: 0,
                            cumulativeCount: 4,
                            rangeDistincts: 0,
                            cumulativeDistincts: 2
                        }
                    ],
                    bounds: ["array", "mixed"]
                },
                minHistogram: {
                    buckets: [
                        {
                            boundaryCount: 3,
                            rangeCount: 0,
                            cumulativeCount: 3,
                            rangeDistincts: 0,
                            cumulativeDistincts: 1
                        },
                    ],
                    bounds: ["array"]
                },
                maxHistogram: {
                    buckets: [
                        {
                            boundaryCount: 2,
                            rangeCount: 0,
                            cumulativeCount: 2,
                            rangeDistincts: 0,
                            cumulativeDistincts: 1
                        },
                        {
                            boundaryCount: 1,
                            rangeCount: 0,
                            cumulativeCount: 3,
                            rangeDistincts: 0,
                            cumulativeDistincts: 2
                        }
                    ],
                    bounds: ["array", "mixed"]
                },
                typeCount: [{typeName: "StringSmall", count: 3}],
            },
            emptyArrayCount: 1,
            trueCount: 0,
            falseCount: 0,
            sampleRate: 1.0,
            documents: 6,
        }
    };

    createAndValidateHistogram({coll, expectedHistogram});

    // Verify CE.
    forceCE("histogram");

    // Equality predicates.
    verifyCEForMatch(
        {coll, predicate: {a: "scalar"}, expected: [{_id: 0, a: "scalar"}], hint: idx});
    verifyCEForMatch({
        coll,
        predicate: {a: "array"},
        expected: [
            {_id: 2, a: ["array"]},
            {_id: 3, a: ["array", "array"]},
            {_id: 4, a: ["array", "mixed"]}
        ],
        hint: idx
    });
    verifyCEForMatch({
        coll,
        predicate: {a: "mixed"},
        expected: [
            {_id: 1, a: "mixed"},
            {_id: 4, a: ["array", "mixed"]},
        ],
        hint: idx
    });
    verifyCEForMatch({coll, predicate: {a: "notAValue"}, expected: [], hint: idx});

    // $elemMatch equality predicates.
    verifyCEForMatch(
        {coll, predicate: {a: {$elemMatch: {$eq: "scalar"}}}, expected: [], hint: idx});
    verifyCEForMatch({
        coll,
        predicate: {a: {$elemMatch: {$eq: "array"}}},
        expected: [
            {_id: 2, a: ["array"]},
            {_id: 3, a: ["array", "array"]},
            {_id: 4, a: ["array", "mixed"]}
        ],
        hint: idx
    });
    verifyCEForMatch({
        coll,
        predicate: {a: {$elemMatch: {$eq: "mixed"}}},
        expected: [
            {_id: 4, a: ["array", "mixed"]},
        ],
        hint: idx
    });
    verifyCEForMatch(
        {coll, predicate: {a: {$elemMatch: {$eq: "notAValue"}}}, expected: [], hint: idx});

    // Estimate some non-$elemMatch range predicates.
    verifyCEForMatch({
        coll,
        predicate: {a: {$lt: "scalar"}},
        expected: [
            {_id: 1, a: "mixed"},
            {_id: 2, a: ["array"]},
            {_id: 3, a: ["array", "array"]},
            {_id: 4, a: ["array", "mixed"]},
        ],
        hint: idx
    });
    verifyCEForMatch({coll, predicate: {a: {$lt: "array"}}, expected: [], hint: idx});
    verifyCEForMatch({
        coll,
        predicate: {a: {$lt: "mixed"}},
        expected: [
            {_id: 2, a: ["array"]},
            {_id: 3, a: ["array", "array"]},
            {_id: 4, a: ["array", "mixed"]},
        ],
        hint: idx
    });
    verifyCEForMatch({
        coll,
        predicate: {a: {$lte: "scalar"}},
        expected: [
            {_id: 0, a: "scalar"},
            {_id: 1, a: "mixed"},
            {_id: 2, a: ["array"]},
            {_id: 3, a: ["array", "array"]},
            {_id: 4, a: ["array", "mixed"]},
        ],
        hint: idx
    });
    verifyCEForMatch({
        coll,
        predicate: {a: {$lte: "array"}},
        expected: [
            {_id: 2, a: ["array"]},
            {_id: 3, a: ["array", "array"]},
            {_id: 4, a: ["array", "mixed"]},
        ],
        hint: idx
    });
    verifyCEForMatch({
        coll,
        predicate: {a: {$lte: "mixed"}},
        expected: [
            {_id: 1, a: "mixed"},
            {_id: 2, a: ["array"]},
            {_id: 3, a: ["array", "array"]},
            {_id: 4, a: ["array", "mixed"]},
        ],
        hint: idx
    });

    // Estimate some $elemMatch range predicates.
    verifyCEForMatch({
        coll,
        predicate: {a: {$elemMatch: {$lt: "scalar"}}},
        expected: [
            {_id: 2, a: ["array"]},
            {_id: 3, a: ["array", "array"]},
            {_id: 4, a: ["array", "mixed"]},
        ],
        // In this case, CE is only seeing one scalar interval. Because we don't get a PathArr
        // interval, we estimate this to include scalars.
        ce: 4,
        hint: idx
    });
    verifyCEForMatch({coll, predicate: {a: {$elemMatch: {$lt: "array"}}}, expected: [], hint: idx});
    verifyCEForMatch({
        coll,
        predicate: {a: {$elemMatch: {$lt: "mixed"}}},
        expected: [
            {_id: 2, a: ["array"]},
            {_id: 3, a: ["array", "array"]},
            {_id: 4, a: ["array", "mixed"]},
        ],
        hint: idx
    });
    verifyCEForMatch({
        coll,
        predicate: {a: {$elemMatch: {$lte: "scalar"}}},
        expected: [
            {_id: 2, a: ["array"]},
            {_id: 3, a: ["array", "array"]},
            {_id: 4, a: ["array", "mixed"]},
        ],
        // In this case, CE is only seeing one scalar interval. Because we don't get a PathArr
        // interval, we estimate this to include scalars.
        ce: 4,
        hint: idx
    });
    verifyCEForMatch({
        coll,
        predicate: {a: {$elemMatch: {$lte: "array"}}},
        expected: [
            {_id: 2, a: ["array"]},
            {_id: 3, a: ["array", "array"]},
            {_id: 4, a: ["array", "mixed"]},
        ],
        hint: idx
    });
    verifyCEForMatch({
        coll,
        predicate: {a: {$elemMatch: {$lte: "mixed"}}},
        expected: [
            {_id: 2, a: ["array"]},
            {_id: 3, a: ["array", "array"]},
            {_id: 4, a: ["array", "mixed"]},
        ],
        // In this case, CE is only seeing one scalar interval. Because we don't get a PathArr
        // interval, we estimate this to include scalars.
        ce: 4,
        hint: idx
    });
});
}());
