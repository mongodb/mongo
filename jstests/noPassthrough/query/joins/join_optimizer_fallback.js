/**
 * Tests that join optimization falls back gracefully in cases when it is not supported.
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe
 * ]
 */
import {joinOptUsed, plannerStageIsJoinOptNode} from "jstests/libs/query/join_utils.js";
import {getWinningPlanFromExplain, getAllPlanStages} from "jstests/libs/query/analyze_plan.js";

let conn = MongoRunner.runMongod({setParameter: {featureFlagPathArrayness: true}});

// Test that cross-DB joins are not accepted by the join optimizer.
const db1 = "test";
const db2 = "test2";
const db3 = "test3";

const coll1 = conn.getDB(db1)[jsTestName() + "_coll1"];
const coll12 = conn.getDB(db1)[jsTestName() + "_coll2"];
const coll13 = conn.getDB(db1)[jsTestName() + "_coll3"];
const coll2 = conn.getDB(db2)[jsTestName() + "_coll2"];
const coll3 = conn.getDB(db3)[jsTestName() + "_coll3"];

coll1.drop();
coll12.drop();
coll13.drop();
coll2.drop();
coll3.drop();

assert.commandWorked(coll1.insertOne({a: 1, b: 1, x: {c: 1}}));
assert.commandWorked(coll12.insertOne({a: 1, b: 1, c: 1, d: "foo"}));
assert.commandWorked(coll13.insertOne({a: 1, b: 1, d: "foo"}));
assert.commandWorked(coll2.insertOne({a: 1, b: 1}));
assert.commandWorked(coll3.insertOne({a: 1, b: 1}));
// Add index for multikeyness info for path arrayness.
assert.commandWorked(coll1.createIndex({dummy: 1, a: 1, b: 1, "x.c": 1}));
assert.commandWorked(coll12.createIndex({dummy: 1, a: 1, b: 1, c: 1, d: 1}));
assert.commandWorked(coll13.createIndex({dummy: 1, a: 1, b: 1, d: 1}));
assert.commandWorked(coll2.createIndex({dummy: 1, a: 1, b: 1}));
assert.commandWorked(coll3.createIndex({dummy: 1, a: 1, b: 1}));

function assertSameResultsWithJoinOptToggled(coll, pipeline, aggOptions, expectedCount) {
    assert.commandWorked(conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
    assert.eq(coll.aggregate(pipeline, aggOptions).toArray().length, expectedCount);
    assert.commandWorked(conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));
    assert.eq(coll.aggregate(pipeline, aggOptions).toArray().length, expectedCount);
}

// This helper is for test cases where the entire pipeline is ineligible for join optimization.
function runTestCaseIneligiblePipeline({coll = coll1, pipeline, aggOptions = {}, expectedCount}) {
    assertSameResultsWithJoinOptToggled(coll, pipeline, aggOptions, expectedCount);
    const explain = coll.explain().aggregate(pipeline, aggOptions);
    assert(!joinOptUsed(explain), "Expected join optimizer and actual usage differ: " + tojson(explain));
}

// This helper is for test cases where the prefix is eligible for join opt but the suffix is not.
function runTestCaseIneligibleSuffix({
    coll = coll1,
    pipeline,
    aggOptions = {},
    expectedCount,
    expectedJoinNodesInPrefix,
}) {
    assertSameResultsWithJoinOptToggled(coll, pipeline, aggOptions, expectedCount);
    const explain = coll.explain().aggregate(pipeline, aggOptions);

    // Since the prefix is join eligible we should see the usedJoinOptimization flag in the explain.
    assert(joinOptUsed(explain), "Expected join optimizer and actual usage differ: " + tojson(explain));
    let stages = getAllPlanStages(getWinningPlanFromExplain(explain));

    // Make sure we see the amount of join nodes in the prefix that we expected.
    // Slice's end index is exclusive eg it will not look at node at index 1.
    stages.slice(0, expectedJoinNodesInPrefix).forEach(function (stage) {
        assert(plannerStageIsJoinOptNode(stage), stage);
    });
    // Make sure there are no join nodes in the suffix.
    stages.slice(expectedJoinNodesInPrefix).forEach(function (stage) {
        assert(!plannerStageIsJoinOptNode(stage), stage);
    });
}

// This helper is for test cases where the entire pipeline is eligible for join optimization.
function runTestCaseEligiblePipeline({coll = coll1, pipeline, aggOptions = {}, expectedCount}) {
    assertSameResultsWithJoinOptToggled(coll, pipeline, aggOptions, expectedCount);
    const explain = coll.explain().aggregate(pipeline, aggOptions);
    assert(joinOptUsed(explain), "Expected join optimizer and actual usage differ: " + tojson(explain));
}

// Cross-db $lookup (eg join collections on different databases) is ineligible for optimization.
runTestCaseIneligiblePipeline({
    pipeline: [
        {
            $lookup: {
                from: {
                    db: db2,
                    coll: coll2.getName(),
                },
                localField: "a",
                foreignField: "a",
                as: "coll2",
            },
        },
        {$unwind: "$coll2"},
        {
            $lookup: {
                from: {
                    db: db3,
                    coll: coll3.getName(),
                },
                localField: "a",
                foreignField: "a",
                as: "coll3",
            },
        },
        {$unwind: "$coll3"},
    ],
    expectedCount: 1,
});

// Prefix is eligible but suffix is cross-DB $lookup and therefore ineligible.
runTestCaseIneligiblePipeline({
    pipeline: [
        {
            $lookup: {
                from: {
                    db: db1,
                    coll: coll12.getName(),
                },
                localField: "a",
                foreignField: "a",
                as: "coll2",
            },
        },
        {$unwind: "$coll2"},
        {
            $lookup: {
                from: {
                    db: db3,
                    coll: coll3.getName(),
                },
                localField: "a",
                foreignField: "a",
                as: "coll3",
            },
        },
        {$unwind: "$coll3"},
    ],
    expectedCount: 1,
});

// Query involving only a cross-product is not accepted by the join optimizer.
runTestCaseIneligiblePipeline({
    pipeline: [
        {
            $lookup: {
                from: coll2.getName(),
                pipeline: [{$match: {a: {$lt: 0}}}],
                as: "coll2",
            },
        },
        {$unwind: "$coll2"},
    ],
    expectedCount: 0,
});

// Prefix eligible and suffix is cross-product $lookup is not accepted by the join optimizer.
runTestCaseIneligiblePipeline({
    pipeline: [
        {
            $lookup: {
                from: coll12.getName(),
                localField: "a",
                foreignField: "a",
                as: "coll12",
            },
        },
        {$unwind: "$coll12"},
        {
            $lookup: {
                from: coll13.getName(),
                pipeline: [{$match: {a: {$gt: 0}}}],
                as: "coll13",
            },
        },
        {$unwind: "$coll13"},
    ],
    expectedCount: 1,
});

// Fallback if the prefix of the pipeline contains a $sort.
runTestCaseIneligiblePipeline({
    pipeline: [
        {$sort: {a: 1}},
        {$lookup: {from: coll12.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
    ],
    expectedCount: 1,
});

runTestCaseIneligiblePipeline({
    pipeline: [
        {$match: {b: 1}},
        {$sort: {a: 1}},
        {$lookup: {from: coll12.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
    ],
    expectedCount: 1,
});

// Fallback if $lookup sub-pipeline contains a $sort.
runTestCaseIneligiblePipeline({
    pipeline: [
        {$lookup: {from: coll12.getName(), as: "x", localField: "a", foreignField: "a", pipeline: [{$sort: {a: 1}}]}},
        {$unwind: "$x"},
    ],
    expectedCount: 1,
});

// Join opt should be applied to the prefix because it is eligible but
// *not* the remaining pipeline because of the $sort in the suffix.
runTestCaseIneligibleSuffix({
    pipeline: [
        {
            $lookup: {
                from: coll12.getName(),
                localField: "a",
                foreignField: "a",
                as: "coll12",
            },
        },
        {$unwind: "$coll12"},
        {$sort: {a: -1}},
        {
            $lookup: {
                from: coll13.getName(),
                pipeline: [{$match: {a: {$gt: 0}}}],
                localField: "b",
                foreignField: "b",
                as: "coll13",
            },
        },
        {$unwind: "$coll13"},
    ],
    expectedCount: 1,
    expectedJoinNodesInPrefix: 1,
});

// Conflicting prefix in the second as field
runTestCaseIneligibleSuffix({
    pipeline: [
        {$lookup: {from: coll12.getName(), as: "x.y", localField: "x.c", foreignField: "c"}},
        {$unwind: "$x.y"},
        {$lookup: {from: coll13.getName(), as: "x.y.z", localField: "x.y.d", foreignField: "d"}},
        {$unwind: "$x.y.z"},
    ],
    expectedCount: 1,
    expectedJoinNodesInPrefix: 1,
});

// Conflicting prefix in the second as field.
runTestCaseIneligibleSuffix({
    pipeline: [
        {$lookup: {from: coll12.getName(), as: "x.y.z", localField: "x.c", foreignField: "c"}},
        {$unwind: "$x.y.z"},
        {$lookup: {from: coll13.getName(), as: "x.y", localField: "x.y.z.d", foreignField: "d"}},
        {$unwind: "$x.y"},
    ],
    expectedCount: 1,
    expectedJoinNodesInPrefix: 1,
});

// Conflicting prefix in the second as field.
runTestCaseIneligibleSuffix({
    pipeline: [
        {$lookup: {from: coll12.getName(), as: "x", localField: "x.c", foreignField: "c"}},
        {$unwind: "$x"},
        {$lookup: {from: coll13.getName(), as: "x.y", localField: "b", foreignField: "a"}},
        {$unwind: "$x.y"},
    ],
    expectedCount: 1,
    expectedJoinNodesInPrefix: 1,
});

// Conflicting prefix in the second as field.
runTestCaseIneligibleSuffix({
    pipeline: [
        {$lookup: {from: coll12.getName(), as: "x.y", localField: "x.c", foreignField: "c"}},
        {$unwind: "$x.y"},
        {$lookup: {from: coll13.getName(), as: "x", localField: "b", foreignField: "a"}},
        {$unwind: "$x"},
    ],
    expectedCount: 1,
    expectedJoinNodesInPrefix: 1,
});

runTestCaseEligiblePipeline({
    pipeline: [
        {$lookup: {from: coll12.getName(), as: "x.y", localField: "x.c", foreignField: "c"}},
        {$unwind: "$x.y"},
        {$lookup: {from: coll13.getName(), as: "x.z", localField: "b", foreignField: "a"}},
        {$unwind: "$x.z"},
    ],
    expectedCount: 1,
});

// $lookup with no join predicate can still be optimized if the rest of the pipeline establishes
// a connected join graph.
runTestCaseEligiblePipeline({
    pipeline: [
        {
            $lookup: {
                from: coll12.getName(),
                as: "coll12",
                pipeline: [],
            },
        },
        {$unwind: "$coll12"},
        {
            $lookup: {
                from: coll13.getName(),
                let: {a: "$a", a12: "$coll12.a"},
                pipeline: [{$match: {$expr: {$and: [{$eq: ["$a", "$$a"]}, {$eq: ["$a", "$$a12"]}]}}}],
                as: "coll13",
            },
        },
        {$unwind: "$coll13"},
    ],
    expectedCount: 1,
});

// Eligible prefix followed by ineligible $unwind with preserveNullAndEmptyArrays.
runTestCaseIneligibleSuffix({
    pipeline: [
        {$lookup: {from: coll12.getName(), as: "coll12", localField: "a", foreignField: "a"}},
        {$unwind: "$coll12"},
        {$lookup: {from: coll13.getName(), as: "coll13", localField: "b", foreignField: "b"}},
        {$unwind: {path: "$coll13", preserveNullAndEmptyArrays: true}},
    ],
    expectedCount: 1,
    expectedJoinNodesInPrefix: 1,
});

// Eligible prefix followed by ineligible $unwind with includeArrayIndex.
runTestCaseIneligibleSuffix({
    pipeline: [
        {$lookup: {from: coll12.getName(), as: "coll12", localField: "a", foreignField: "a"}},
        {$unwind: "$coll12"},
        {$lookup: {from: coll13.getName(), as: "coll13", localField: "b", foreignField: "b"}},
        {$unwind: {path: "$coll13", includeArrayIndex: "idx"}},
    ],
    expectedCount: 1,
    expectedJoinNodesInPrefix: 1,
});

// Fallback when $unwind has both preserveNullAndEmptyArrays and includeArrayIndex.
runTestCaseIneligiblePipeline({
    pipeline: [
        {$lookup: {from: coll12.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: {path: "$x", preserveNullAndEmptyArrays: true, includeArrayIndex: "idx"}},
    ],
    expectedCount: 1,
});

// Aggregation is ineligible when a hint is specified.
assert.commandWorked(coll1.createIndex({a: 1}));
assert.commandWorked(coll12.createIndex({a: 1}));

runTestCaseIneligiblePipeline({
    pipeline: [
        {
            $lookup: {
                from: coll12.getName(),
                localField: "a",
                foreignField: "a",
                as: "coll12",
            },
        },
        {$unwind: "$coll12"},
    ],
    aggOptions: {hint: "a_1"},
    expectedCount: 1,
});

runTestCaseIneligiblePipeline({
    pipeline: [
        {
            $lookup: {
                from: coll12.getName(),
                localField: "a",
                foreignField: "a",
                as: "coll12",
            },
        },
        {$unwind: "$coll12"},
    ],
    aggOptions: {hint: {$natural: 1}},
    expectedCount: 1,
});

runTestCaseEligiblePipeline({
    pipeline: [
        {
            $lookup: {
                from: coll12.getName(),
                localField: "a",
                foreignField: "a",
                as: "coll12",
            },
        },
        {$unwind: "$coll12"},
    ],
    aggOptions: {hint: {}},
    expectedCount: 1,
});

// Rooted $ors on the base collection disqualify the query completely.
runTestCaseIneligiblePipeline({
    pipeline: [
        {$match: {$or: [{a: 1}, {a: 2}, {a: {$gt: 12}}]}},
        {
            $lookup: {
                from: coll12.getName(),
                localField: "a",
                foreignField: "a",
                as: "coll12",
            },
        },
        {$unwind: "$coll12"},
    ],
    expectedCount: 1,
});

// Same goes for a rooted $or on the first collection we're joining on.
runTestCaseIneligiblePipeline({
    pipeline: [
        {
            $lookup: {
                from: coll12.getName(),
                localField: "a",
                foreignField: "a",
                as: "coll12",
                pipeline: [{$match: {$or: [{a: 1}, {a: 2}, {a: {$gt: 12}}]}}],
            },
        },
        {$unwind: "$coll12"},
    ],
    expectedCount: 1,
});

// But, we will permit a prefix to a rooted-$or query.
runTestCaseIneligibleSuffix({
    pipeline: [
        {
            $lookup: {
                from: coll12.getName(),
                localField: "a",
                foreignField: "a",
                as: "coll12",
            },
        },
        {$unwind: "$coll12"},
        {
            $lookup: {
                from: coll13.getName(),
                let: {
                    "a": "$a",
                },
                pipeline: [{$match: {$or: [{a: 1}, {a: 2}, {a: {$gt: 12}}], $expr: {$eq: ["$$a", "$a"]}}}],
                as: "coll13",
            },
        },
        {$unwind: "$coll13"},
    ],
    expectedCount: 1,
    expectedJoinNodesInPrefix: 1,
});

// We don't bail out if our predicate is too complex to run as a rooted-$or.
runTestCaseIneligibleSuffix({
    pipeline: [
        {
            $lookup: {
                from: coll12.getName(),
                localField: "a",
                foreignField: "a",
                as: "coll12",
            },
        },
        {$unwind: "$coll12"},
        {
            $lookup: {
                from: coll13.getName(),
                let: {
                    "a": "$a",
                },
                pipeline: [{$match: {$or: [{a: 1}, {a: 2}, {a: {$gt: 12}}], $expr: {$eq: ["$$a", "$a"]}, d: "foo"}}],
                as: "coll13",
            },
        },
        {$unwind: "$coll13"},
    ],
    expectedCount: 1,
    expectedJoinNodesInPrefix: 2,
});

// Numeric path in join predicate falls back gracefully even when the path is indexed.
// An index on "a.0" is not multikey (numeric components always address a single array element),
// but the join optimizer must still fall back because numeric paths are ineligible predicates.
{
    const collNumeric = conn.getDB(db1)[jsTestName() + "_numeric"];
    collNumeric.drop();
    assert.commandWorked(collNumeric.insertOne({a: [1, 2], b: 1}));

    // Index on "a.0": valid and not multikey because "a.0" always returns a single element.
    assert.commandWorked(collNumeric.createIndex({"a.0": 1}));

    // localField/foreignField syntax: a.0 (numeric) = a.
    runTestCaseIneligiblePipeline({
        pipeline: [
            {$lookup: {from: collNumeric.getName(), localField: "a", foreignField: "a.0", as: "joined"}},
            {$unwind: "$joined"},
        ],
        expectedCount: 1,
    });

    // $expr syntax.
    runTestCaseIneligiblePipeline({
        pipeline: [
            {
                $lookup: {
                    from: collNumeric.getName(),
                    let: {aa: "$a"},
                    pipeline: [{$match: {$expr: {$eq: ["$$aa", "$a.0"]}}}],
                    as: "joined",
                },
            },
            {$unwind: "$joined"},
        ],
        expectedCount: 1,
    });

    // Repeat, but now changing the base coll to be numeric.
    runTestCaseIneligiblePipeline({
        coll: collNumeric,
        pipeline: [
            {$lookup: {from: coll1.getName(), localField: "a.0", foreignField: "a", as: "joined"}},
            {$unwind: "$joined"},
        ],
        expectedCount: 1,
    });

    runTestCaseIneligiblePipeline({
        coll: collNumeric,
        pipeline: [
            {
                $lookup: {
                    from: coll1.getName(),
                    let: {aa: "$a.0"},
                    pipeline: [{$match: {$expr: {$eq: ["$$aa", "$a"]}}}],
                    as: "joined",
                },
            },
            {$unwind: "$joined"},
        ],
        expectedCount: 0,
    });

    // Validate trailing $match case.
    runTestCaseIneligiblePipeline({
        pipeline: [
            {
                $lookup: {
                    from: collNumeric.getName(),
                    pipeline: [],
                    as: "joined",
                },
            },
            {$unwind: "$joined"},
            {$match: {$expr: {$eq: ["$joined.a.0", "$a"]}}},
        ],
        expectedCount: 0,
    });

    runTestCaseIneligiblePipeline({
        coll: collNumeric,
        pipeline: [
            {
                $lookup: {
                    from: coll1.getName(),
                    pipeline: [],
                    as: "joined",
                },
            },
            {$unwind: "$joined"},
            {$match: {$expr: {$eq: ["$joined.a", "$a.0"]}}},
        ],
        expectedCount: 0,
    });
}

MongoRunner.stopMongod(conn);
