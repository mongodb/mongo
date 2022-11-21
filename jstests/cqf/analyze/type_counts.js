/**
 * This is an integration test for histogram CE & statistics to ensure that we can create a
 * histogram with appropriate type counts and retrieve that histogram to estimate a simple match
 * predicate. Note that this tests predicates and histograms on several types.
 */
(function() {
"use strict";

load('jstests/libs/ce_stats_utils.js');

runHistogramsTest(function testTypeCounts() {
    const coll = db.type_counts;

    // Set up collection to test a simple field path.
    coll.drop();
    assert.commandWorked(coll.createIndex({"a": 1}));
    assert.commandWorked(coll.insertMany([
        /* Booleans: 1 true, 2 false. */
        {_id: 0, a: true},
        {_id: 1, a: false},
        {_id: 2, a: false},
        /* Null: 3 null, 2 missing. */
        {_id: 3, a: null},
        {_id: 4, a: null},
        {_id: 5, a: null},
        {_id: 6, b: 2},
        {_id: 7},
        /* Objects: 3. */
        {_id: 8, a: {a: 1, b: 2}},
        {_id: 9, a: {}},
        {_id: 10, a: {c: 3}},
        /* Arrays: 4 empty, 1 not. */
        {_id: 11, a: []},
        {_id: 12, a: []},
        {_id: 13, a: []},
        {_id: 14, a: []},
        // TODO: SERVER-71513 this should generate array histograms.
        {_id: 15, a: [1, 2, 3]},
    ]));

    createAndValidateHistogram({
        coll,
        expectedHistogram: {
            _id: "a",
            statistics: {
                typeCount: [
                    {typeName: "Boolean", count: 3},
                    {typeName: "Null", count: 3},
                    {typeName: "Nothing", count: 2},
                    {typeName: "Object", count: 3},
                    {typeName: "Array", count: 5},
                ],
                scalarHistogram: {buckets: [], bounds: []},
                emptyArrayCount: 4,
                trueCount: 1,
                falseCount: 2,
                documents: 16,
            }
        }
    });

    // Verify type count CE.
    forceHistogramCE();
    let hint = {a: 1};

    // TODO SERVER-70936: estimate boolean counts.
    // verifyCEForMatch({coll, predicate: {a: true}, expected: [{_id: 0, a: true}], ce: 1, hint});
    // verifyCEForMatch({
    //     coll,
    //     predicate: {a: false},
    //     expected: [{_id: 1, a: false}, {_id: 2, a: false}],
    //     ce: 2,
    //     hint
    // });

    // If we hint the index {a: 1} for this query, we don't get an IndexScan plan; instead, we fail
    // to optimize. It looks like we can't test CE for this case because we only generate a
    // PhysicalScan plan. verifyCEForMatch({
    //     coll,
    //     predicate: {a: {$eq: []}},
    //     expected: [
    //         {_id: 11, a: []},
    //         {_id: 12, a: []},
    //         {_id: 13, a: []},
    //         {_id: 14, a: []},
    //     ],
    //     ce: 4,
    //     hint
    // });

    // TODO SERVER-71513: Validate estimate for non-empty arrays.

    // Currently, we always estimate any object predicate as the total count of objects.
    verifyCEForMatch(
        {coll, predicate: {a: {a: 1, b: 2}}, expected: [{_id: 8, a: {a: 1, b: 2}}], ce: 3, hint});
    verifyCEForMatch({coll, predicate: {a: {}}, expected: [{_id: 9, a: {}}], ce: 3, hint});
    verifyCEForMatch({coll, predicate: {a: {c: 3}}, expected: [{_id: 10, a: {c: 3}}], ce: 3, hint});
    verifyCEForMatch({coll, predicate: {a: {notInColl: 1}}, expected: [], ce: 3, hint});

    // Test null predicate match. TODO SERVER-71377: make estimate include missing values.
    verifyCEForMatch({
        coll,
        predicate: {a: null},
        expected: [
            {_id: 3, a: null},
            {_id: 4, a: null},
            {_id: 5, a: null},
            {_id: 6, b: 2},
            {_id: 7},
        ],
        ce: 3,
        hint
    });

    // Set up collection to test a more complex field path.
    coll.drop();
    assert.commandWorked(coll.createIndex({"a.b": 1}));
    assert.commandWorked(coll.insertMany([
        /* Booleans: 2 true, 1 false. */
        {_id: 0, a: {b: true}},
        {_id: 1, a: {b: true}},
        {_id: 2, a: {b: false}},
        /* Null: 1 null, 8 missing. */
        {_id: 3, a: {b: null}},
        {_id: 4, a: {}},
        {_id: 5, a: {c: 3}},
        {_id: 6, a: 1},
        {_id: 7, a: null},
        {_id: 8, b: 2},
        {_id: 9},
        {_id: 10, "a.b": 1},
        {_id: 11, "a.b.c": 1},
        /* Scalar: 2. */
        {_id: 12, a: {b: NumberInt(1)}},
        {_id: 13, a: {a: 1, b: NumberInt(2)}},
        /* Object: 2. */
        {_id: 14, a: {b: {}}},
        {_id: 15, a: {b: {c: 1}}},
        /* Arrays: 1 empty, 2 not. */
        {_id: 16, a: {b: []}},
        // TODO: SERVER-71513 these should generate array histograms.
        {_id: 17, a: {b: [1, 2, 3]}},
        {_id: 18, a: [{b: 1}, {b: 2}]},
    ]));

    createAndValidateHistogram({
        coll,
        expectedHistogram: {
            _id: "a.b",
            statistics: {
                typeCount: [
                    {typeName: "NumberInt32", count: 2},
                    {typeName: "Boolean", count: 3},
                    {typeName: "Null", count: 1},
                    {typeName: "Nothing", count: 8},
                    {typeName: "Object", count: 2},
                    {typeName: "Array", count: 3},
                ],
                scalarHistogram: {
                    buckets: [
                        {
                            boundaryCount: 1,
                            rangeCount: 0,
                            rangeDistincts: 0,
                            cumulativeCount: 1,
                            cumulativeDistincts: 1
                        },
                        {
                            boundaryCount: 1,
                            rangeCount: 0,
                            rangeDistincts: 0,
                            cumulativeCount: 2,
                            cumulativeDistincts: 2
                        }
                    ],
                    bounds: [1, 2]
                },
                // TODO: SERVER-71513 add arrayHistogram field.
                emptyArrayCount: 1,
                trueCount: 2,
                falseCount: 1,
                documents: 19,
            }
        }
    });

    // Verify type count CE.
    forceHistogramCE();
    hint = {"a.b": 1};

    // Test scalar CE. TODO SERVER-71513: update this to include array estimate.
    verifyCEForMatch({
        coll,
        predicate: {"a.b": 1},
        expected: [
            {_id: 12, a: {b: 1}},
            {_id: 17, a: {b: [1, 2, 3]}},
            {_id: 18, a: [{b: 1}, {b: 2}]},
        ],
        ce: 1,
        hint
    });

    // TODO SERVER-70936: estimate boolean counts.
    // TODO SERVER-71513: Validate estimate for non-empty arrays.

    // Currently, we always estimate any object predicate as the total count of objects.
    verifyCEForMatch(
        {coll, predicate: {"a.b": {}}, expected: [{_id: 14, a: {b: {}}}], ce: 2, hint});
    verifyCEForMatch(
        {coll, predicate: {"a.b": {c: 1}}, expected: [{_id: 15, a: {b: {c: 1}}}], ce: 2, hint});
    verifyCEForMatch({coll, predicate: {"a.b": {c: 2}}, expected: [], ce: 2, hint});

    // Test null predicate match. TODO SERVER-71377: make estimate include missing values.
    verifyCEForMatch({
        coll,
        predicate: {"a.b": null},
        expected: [
            {_id: 3, a: {b: null}},
            {_id: 4, a: {}},
            {_id: 5, a: {c: 3}},
            {_id: 6, a: 1},
            {_id: 7, a: null},
            {_id: 8, b: 2},
            {_id: 9},
            {_id: 10, "a.b": 1},
            {_id: 11, "a.b.c": 1},
        ],
        ce: 1,
        hint
    });
});
}());
