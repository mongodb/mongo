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
    assert.commandWorked(coll.createIndex({a: 1}));
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
                // Generated from the array [1, 2, 3].
                arrayStatistics: {
                    uniqueHistogram: {
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
                            },
                            {
                                boundaryCount: 1,
                                rangeCount: 0,
                                cumulativeCount: 3,
                                rangeDistincts: 0,
                                cumulativeDistincts: 3
                            }
                        ],
                        bounds: [1, 2, 3]
                    },
                    minHistogram: {
                        buckets: [{
                            boundaryCount: 1,
                            rangeCount: 0,
                            cumulativeCount: 1,
                            rangeDistincts: 0,
                            cumulativeDistincts: 1
                        }],
                        bounds: [1]
                    },
                    maxHistogram: {
                        buckets: [{
                            boundaryCount: 1,
                            rangeCount: 0,
                            cumulativeCount: 1,
                            rangeDistincts: 0,
                            cumulativeDistincts: 1
                        }],
                        bounds: [3]
                    },
                    typeCount: [{typeName: "NumberDouble", count: 3}],
                },
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
    // PhysicalScan plan.
    // verifyCEForMatch({
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

    // Validate estimates for non-empty arrays.
    verifyCEForMatch({coll, predicate: {a: 1}, expected: [{_id: 15, a: [1, 2, 3]}], hint});
    verifyCEForMatch(
        {coll, predicate: {a: {$elemMatch: {$eq: 1}}}, expected: [{_id: 15, a: [1, 2, 3]}], hint});
    verifyCEForMatch({coll, predicate: {a: 2}, expected: [{_id: 15, a: [1, 2, 3]}], hint});
    verifyCEForMatch(
        {coll, predicate: {a: {$elemMatch: {$eq: 2}}}, expected: [{_id: 15, a: [1, 2, 3]}], hint});
    verifyCEForMatch({coll, predicate: {a: 3}, expected: [{_id: 15, a: [1, 2, 3]}], hint});
    verifyCEForMatch(
        {coll, predicate: {a: {$elemMatch: {$eq: 3}}}, expected: [{_id: 15, a: [1, 2, 3]}], hint});
    verifyCEForMatch({coll, predicate: {a: 4}, expected: [], hint});
    verifyCEForMatch({coll, predicate: {a: {$elemMatch: {$eq: 4}}}, expected: [], hint});

    // The plan generated in the following two cases has the following shape:
    //
    //  Root (CE: 7)
    //  |
    //  Filter (CE: 7)
    //  |
    //  Binary Join (CE: 16)
    //  |      |
    //  |      GroupBy (CE: 1)
    //  |      |
    //  |      Union (CE: 1) ___
    //  |      |                |
    //  |      Ixscan (CE: 1)   Ixscan (CE: 1)
    //  |      [1, 1]           [[1, 2, 3], [1, 2, 3]]
    //  |
    //  LimitSkip (CE: 16)
    //  |
    //  Seek (CE: 16)
    //
    // We only care about the Ixscan estimation for now, so those nodes are the only ones we will
    // verify in this case. Based on the logs, it looks like we estimate the sargable node with both
    // intervals together, and attach this estimate to both index scans and the union node above
    // them. Therefore, we only look at the union node estimate in the test.
    // Note that the intervals are combined disjunctively.
    function getUnionNodeCE(explain) {
        const union = navigateToPlanPath(explain, "child.child.leftChild.child");
        assert.neq(union, null, tojson(explain));
        assert.eq(union.nodeType, "Union", tojson(union));
        return [extractLogicalCEFromNode(union)];
    }
    verifyCEForMatchNodes({
        coll,
        predicate: {a: [1, 2, 3]},
        expected: [{_id: 15, a: [1, 2, 3]}],
        hint,
        getNodeCEs: getUnionNodeCE,
        CEs: [1],
    });
    verifyCEForMatchNodes({
        coll,
        predicate: {a: [1, 2]},
        expected: [],
        hint,
        getNodeCEs: getUnionNodeCE,
        CEs: [1],
    });

    verifyCEForMatch({coll, predicate: {a: {$elemMatch: {$eq: [1, 2, 3]}}}, expected: [], hint});
    verifyCEForMatch({coll, predicate: {a: {$elemMatch: {$eq: [1, 2]}}}, expected: [], hint});

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
                arrayStatistics: {
                    uniqueHistogram: {
                        buckets: [
                            {
                                boundaryCount: 2,
                                rangeCount: 0,
                                cumulativeCount: 2,
                                rangeDistincts: 0,
                                cumulativeDistincts: 1
                            },
                            {
                                boundaryCount: 2,
                                rangeCount: 0,
                                cumulativeCount: 4,
                                rangeDistincts: 0,
                                cumulativeDistincts: 2
                            },
                            {
                                boundaryCount: 1,
                                rangeCount: 0,
                                cumulativeCount: 5,
                                rangeDistincts: 0,
                                cumulativeDistincts: 3
                            }
                        ],
                        bounds: [1, 2, 3]
                    },
                    minHistogram: {
                        buckets: [{
                            boundaryCount: 2,
                            rangeCount: 0,
                            cumulativeCount: 2,
                            rangeDistincts: 0,
                            cumulativeDistincts: 1
                        }],
                        bounds: [1]
                    },
                    maxHistogram: {
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
                            },
                        ],
                        bounds: [2, 3]
                    },
                    typeCount: [{typeName: "NumberDouble", count: 5}],
                },
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

    // Test CE for histogrammable types.
    verifyCEForMatch({
        coll,
        predicate: {"a.b": 1},
        expected: [
            {_id: 12, a: {b: 1}},
            {_id: 17, a: {b: [1, 2, 3]}},
            {_id: 18, a: [{b: 1}, {b: 2}]},
        ],
        hint
    });

    // For the two asserts below, the PathArr interval was split out of the SargableNode, and the
    // resulting index scan is estimated as a non-$elemMatch predicate.
    verifyCEForMatchNodes({
        coll,
        predicate: {"a.b": {$elemMatch: {$eq: 1}}},
        expected: [
            {_id: 17, a: {b: [1, 2, 3]}},
        ],
        getNodeCEs: (explain) => {
            // This index scan matches [1, 1] only, and is estimated as such.
            const ixScan = navigateToPlanPath(explain, "child.child.leftChild");
            assert.neq(ixScan, null, tojson(explain));
            assert.eq(ixScan.nodeType, "IndexScan", tojson(ixScan));
            return [extractLogicalCEFromNode(ixScan)];
        },
        CEs: [3],
        hint
    });
    verifyCEForMatchNodes({
        coll,
        predicate: {"a.b": {$elemMatch: {$lt: 3}}},
        expected: [
            {_id: 17, a: {b: [1, 2, 3]}},
        ],
        getNodeCEs: (explain) => {
            // This index scan matches [nan, 3] only, and is estimated as such.
            const ixScan = navigateToPlanPath(explain, "child.child.leftChild.child");
            assert.neq(ixScan, null, tojson(explain));
            assert.eq(ixScan.nodeType, "IndexScan", tojson(ixScan));
            return [extractLogicalCEFromNode(ixScan)];
        },
        CEs: [4],
        hint
    });

    // TODO SERVER-70936: estimate boolean counts.

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

    // Set up a collection to test CE for nested arrays and non-histogrammable types in arrays.
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.insertMany([
        /* Booleans. */
        {_id: 0, a: true},
        {_id: 1, a: false},
        {_id: 2, a: [true]},
        {_id: 3, a: [false]},
        {_id: 4, a: [true, false]},
        {_id: 5, a: [false, false, false]},
        {_id: 6, a: [[false, false], true]},
        {_id: 7, a: [[true]]},
        {_id: 8, a: [[false]]},
        /* Objects. */
        {_id: 9, a: {}},
        {_id: 10, a: [{}]},
        {_id: 11, a: [[{}]]},
        {_id: 12, a: {b: 1}},
        {_id: 13, a: [{c: 1}]},
        {_id: 14, a: [{c: 1}, {e: 1}]},
        {_id: 15, a: [[{d: 1}]]},
        /* Arrays. */
        {_id: 16, a: []},
        {_id: 17, a: [[]]},
        {_id: 18, a: [[[]]]},
        {_id: 19, a: ["a", "b", "c"]},
        {_id: 20, a: [["a", "b", "c"]]},
        {_id: 21, a: [[["a", "b", "c"]]]},
        /* Nulls. */
        {_id: 22},
        {_id: 23, a: null},
        {_id: 24, a: [null]},
        {_id: 25, a: [null, null, null]},
        {_id: 26, a: [[null]]},
        /* Mixed array type-counts. */
        {_id: 27, a: [null, true, false, [], [1, 2, 3], ["a", "b", "c"], {a: 1}, {}]},
    ]));

    // TODO SERVER-71057: Only count types once per array.
    createAndValidateHistogram({
        coll,
        expectedHistogram: {
            _id: "a",
            statistics: {
                typeCount: [
                    {typeName: "Boolean", count: 2},
                    {typeName: "Null", count: 1},
                    {typeName: "Nothing", count: 1},
                    {typeName: "Object", count: 2},
                    {typeName: "Array", count: 22},
                ],
                scalarHistogram: {buckets: [], bounds: []},
                arrayStatistics: {
                    uniqueHistogram: {
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
                            },
                            {
                                boundaryCount: 1,
                                rangeCount: 0,
                                cumulativeCount: 3,
                                rangeDistincts: 0,
                                cumulativeDistincts: 3
                            }
                        ],
                        bounds: ["a", "b", "c"]
                    },
                    minHistogram: {
                        buckets: [{
                            boundaryCount: 1,
                            rangeCount: 0,
                            cumulativeCount: 1,
                            rangeDistincts: 0,
                            cumulativeDistincts: 1
                        }],
                        bounds: ["a"]
                    },
                    maxHistogram: {
                        buckets: [
                            {
                                boundaryCount: 1,
                                rangeCount: 0,
                                cumulativeCount: 1,
                                rangeDistincts: 0,
                                cumulativeDistincts: 1
                            },
                        ],
                        bounds: ["c"]
                    },
                    typeCount: [
                        {typeName: "StringSmall", count: 3},
                        {typeName: "Boolean", count: 10},
                        {typeName: "Null", count: 5},
                        {typeName: "Object", count: 6},
                        {typeName: "Array", count: 13},
                    ],
                },
                emptyArrayCount: 1,
                trueCount: 1,
                falseCount: 1,
                documents: 28,
            }
        }
    });

    // Verify type count CE. Note that for non-$elemMatch preidcates, we include both array and
    // scalar type-counts, while for $elemMatch predicates, we include only array type counts in
    // our estimate.
    forceHistogramCE();
    hint = {a: 1};

    // TODO SERVER-70936: estimate boolean counts.

    // Currently, we always estimate any object predicate as the total count of objects.
    verifyCEForMatch({
        coll,
        predicate: {a: {}},
        expected: [
            {_id: 9, a: {}},
            {_id: 10, a: [{}]},
            {_id: 27, a: [null, true, false, [], [1, 2, 3], ["a", "b", "c"], {a: 1}, {}]},
        ],
        ce: 8,
        hint
    });
    verifyCEForMatch({
        coll,
        predicate: {a: {a: 1}},
        expected: [{_id: 27, a: [null, true, false, [], [1, 2, 3], ["a", "b", "c"], {a: 1}, {}]}],
        ce: 8,
        hint
    });
    verifyCEForMatch({coll, predicate: {a: {b: 1}}, expected: [{_id: 12, a: {b: 1}}], ce: 8, hint});

    // In the $elemMatch case for object predicates, we only include the array object counter value
    // in our estimate.
    verifyCEForMatch({
        coll,
        predicate: {a: {$elemMatch: {$eq: {}}}},
        expected: [
            {_id: 10, a: [{}]},
            {_id: 27, a: [null, true, false, [], [1, 2, 3], ["a", "b", "c"], {a: 1}, {}]},
        ],
        ce: 6,
        hint
    });
    verifyCEForMatch({
        coll,
        predicate: {a: {$elemMatch: {$eq: {a: 1}}}},
        expected: [{_id: 27, a: [null, true, false, [], [1, 2, 3], ["a", "b", "c"], {a: 1}, {}]}],
        ce: 6,
        hint
    });
    verifyCEForMatch(
        {coll, predicate: {a: {$elemMatch: {$eq: {b: 1}}}}, expected: [], ce: 6, hint});

    // We are estimating the following predicates as two intervals joined by a conjunction:
    //  1. [{}, {}] - estimated as the sum of scalar and array type counts 6 + 2 = 8.
    //  2. [[{}], [{}]] - estimated as the count of nested arrays, 13.
    // The disjunction selectivities are then combined via exponential backoff. This estimate can be
    // found at the union of the two index scan nodes in the plan. However, the root node estimate
    // differs due to the filter node & group by node above the union, so we directly verify the
    // estimate of the sargable nodes together.
    verifyCEForMatchNodes({
        coll,
        predicate: {a: [{}]},
        expected: [
            {_id: 10, a: [{}]},
            {_id: 11, a: [[{}]]},
        ],
        getNodeCEs: getUnionNodeCE,
        CEs: [15.323],
        hint
    });
    verifyCEForMatchNodes({
        coll,
        predicate: {a: [{c: 1}]},
        expected: [{_id: 13, a: [{c: 1}]}],
        getNodeCEs: getUnionNodeCE,
        CEs: [15.323],
        hint
    });
    verifyCEForMatchNodes({
        coll,
        predicate: {a: [{d: 1}]},
        expected: [{_id: 15, a: [[{d: 1}]]}],
        getNodeCEs: getUnionNodeCE,
        CEs: [15.323],
        hint
    });

    // Verify CE using array histogram.
    verifyCEForMatch({coll, predicate: {a: ""}, expected: [], hint});
    verifyCEForMatch({coll, predicate: {a: "a"}, expected: [{_id: 19, a: ["a", "b", "c"]}], hint});
    verifyCEForMatch({coll, predicate: {a: "b"}, expected: [{_id: 19, a: ["a", "b", "c"]}], hint});
    verifyCEForMatch({coll, predicate: {a: "c"}, expected: [{_id: 19, a: ["a", "b", "c"]}], hint});
    verifyCEForMatch({coll, predicate: {a: "d"}, expected: [], hint});

    // We estimate nested arrays as the total count of nested arrays.
    verifyCEForMatch({coll, predicate: {a: [""]}, expected: [], ce: /*13,*/ 9.583, hint});
    verifyCEForMatch({
        coll,
        predicate: {a: {$elemMatch: {$eq: ["a", "b", "c"]}}},
        expected: [
            {_id: 20, a: [["a", "b", "c"]]},
            {_id: 27, a: [null, true, false, [], [1, 2, 3], ["a", "b", "c"], {a: 1}, {}]},
        ],
        ce: 13,
        hint
    });
    verifyCEForMatch({
        coll,
        predicate: {a: {$elemMatch: {$eq: [["a", "b", "c"]]}}},
        expected: [{_id: 21, a: [[["a", "b", "c"]]]}],
        ce: 13,
        hint
    });

    // In the following cases, we have two intervals:
    //  1. ["a", "a"] - This is estimated as 1 based on the array buckets.
    //  2. [["a", "b", "c"], ["a", "b", "c"]] - this is estimated as the count of nested arrays: 13.
    // The selectivities are then combined by disjunctive exponential backoff. Once again, we can
    // find this estimate in the Union node.
    verifyCEForMatchNodes({
        coll,
        predicate: {a: ["a", "b", "c"]},
        expected: [
            {_id: 19, a: ["a", "b", "c"]},
            {_id: 20, a: [["a", "b", "c"]]},
            {_id: 27, a: [null, true, false, [], [1, 2, 3], ["a", "b", "c"], {a: 1}, {}]},
        ],
        getNodeCEs: getUnionNodeCE,
        CEs: [13.270],
        hint
    });
    verifyCEForMatchNodes({
        coll,
        predicate: {a: ["a"]},
        expected: [],
        getNodeCEs: getUnionNodeCE,
        CEs: [13.270],
        hint
    });

    // In the following cases, we have two array intervals, each estimated as the count of nested
    // arrays, with the selectivities combined by disjunctive exponential backoff. Once again, we
    // can find this estimate in the Union node.
    verifyCEForMatchNodes({
        coll,
        predicate: {a: [["a", "b", "c"]]},
        expected: [{_id: 20, a: [["a", "b", "c"]]}, {_id: 21, a: [[["a", "b", "c"]]]}],
        getNodeCEs: getUnionNodeCE,
        CEs: [17.021],
        hint
    });
    verifyCEForMatchNodes({
        coll,
        predicate: {a: [[["a", "b", "c"]]]},
        expected: [{_id: 21, a: [[["a", "b", "c"]]]}],
        getNodeCEs: getUnionNodeCE,
        CEs: [17.021],
        hint
    });

    // Verify null CE. TODO SERVER-71377: make estimate include missing values.
    // TODO SERVER-71057: Only count each null once per array.
    verifyCEForMatch({
        coll,
        predicate: {a: null},
        expected: [
            {_id: 22},
            {_id: 23, a: null},
            {_id: 24, a: [null]},
            {_id: 25, a: [null, null, null]},
            {_id: 27, a: [null, true, false, [], [1, 2, 3], ["a", "b", "c"], {a: 1}, {}]},
        ],
        ce: 6,
        hint
    });
    verifyCEForMatch({
        coll,
        predicate: {a: {$elemMatch: {$eq: null}}},
        expected: [
            {_id: 24, a: [null]},
            {_id: 25, a: [null, null, null]},
            {_id: 27, a: [null, true, false, [], [1, 2, 3], ["a", "b", "c"], {a: 1}, {}]},
        ],
        ce: 5,
        hint
    });

    // TODO: we only ever get a PhysicalScan plan in this case. Hinting causes optimization to
    // fail. We estimate scalar empty arrays as 1 correctly, but we also match nested empty
    // arrays- so we also count all nested empty arrays.
    // verifyCEForMatch({coll, predicate: {a: []}, expected: [
    //     {_id: 16, a: []},
    //     {_id: 17, a: [[]]},
    //     {_id: 27, a: [null, true, false, [], [1, 2, 3], ["a", "b", "c"], {a: 1}, {}]},
    // ], ce: 14, hint});

    // In the following case, we expect to count only nested empty arrays.
    verifyCEForMatch({
        coll,
        predicate: {a: {$elemMatch: {$eq: []}}},
        expected: [
            {_id: 17, a: [[]]},
            {_id: 27, a: [null, true, false, [], [1, 2, 3], ["a", "b", "c"], {a: 1}, {}]},
        ],
        ce: 13,
        hint
    });
});
}());
