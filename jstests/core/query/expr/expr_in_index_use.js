// Confirms expected index use when performing a match with a $expr + $in. An eligible $expr + $in
// expression that is rewritten to a MatchExpression $in that excludes array values from
// comparison can take advantage of indexes. The requirement is that each element in the inList is
// scalar, non-null, and not a regex. Arrays, null values, and regexes are handled differently in
// MatchExpression semantics compared to agg, so the expression will not be rewritten or be able to
// use indexes when they are present in the inList.
// @tags: [
//   assumes_read_concern_local,
//   requires_fcv_81,
//   requires_getmore,
// ]

import {
    everyWinningPlan,
    getAggPlanStage,
    getAggPlanStages,
    getEngine,
    getPlanStage,
    hasRejectedPlans
} from "jstests/libs/query/analyze_plan.js";

const coll = db.expr_in_index_use;
coll.drop();

const docs = [
    {_id: 0, category: "clothing"},
    {_id: 1, category: "electronics"},
    {_id: 2, category: "materials"},
    {_id: 3},
    {_id: 4, category: null},
    {_id: 5, category: {}},
    {_id: 6, category: []},
    {_id: 7, category: [[]]},
    {_id: 8, category: [null]},
    {_id: 9, category: [[], [""]]},
    {_id: 10, category: ["clothing", "electronics"]},
    {_id: 11, category: ["materials", "clothing", "electronics"]},
    {_id: 12, category: [["clothing"], ["electronics"]]},
    {_id: 13, category: /clothing/},
    {_id: 14, category: /electronics/},
    {_id: 15, category: [{}]},
    {_id: 16, category: [["clothing", "electronics"]]},
    {_id: 17, category: [[["clothing", "materials", "electronics"]]]},
    {_id: 18, category: [[["clothing", "electronics"]]]},
    {_id: 19, category: [[[null]]]},
    {_id: 20, category: "clothings"},
    {_id: 21, category: 1},
    {_id: 22, category: 1.0},
    {_id: 23, category: NumberDecimal("1.00000000000000")},
    {_id: 24, category: NumberLong(1)},
    {_id: 25, category: {$toDouble: 1}},
    {_id: 26, "category.a": "clothing"},
    {_id: 27, category: {a: "clothing"}},
    {_id: 28, category: {b: "clothing"}},
    {_id: 29, category: [{a: "clothing"}, {a: "electronics"}, {}]}
];
assert.commandWorked(coll.insertMany(docs));

// Create indexes on "category" and "category.a"
assert.commandWorked(coll.createIndexes([{category: 1}, {"category.a": 1}]));

/**
 * Executes the expression 'expr' as both a find and an aggregate. Then confirms
 * 'metricsToCheck', which is an object containing:
 *  - expectedIds:      The ids of the documents the pipeline is expected to return.
 *  - expectedIndex:    Either an index specification object when index use is expected or
 *                      'null' if a collection scan is expected.
 */
function confirmExpectedExprExecution(expr, metricsToCheck) {
    assert(metricsToCheck.hasOwnProperty("expectedRes"),
           "metricsToCheck must contain an expectedRes field");

    // Verify that $expr returns the expected results when run inside the $match stage of an
    // aggregate. Note that assert.sameMembers is used instead of eq because there is no guarantee
    // on the order documents are returned in sharded suites.
    const pipeline = [{$match: {$expr: expr}}];
    assert.sameMembers(metricsToCheck.expectedRes, coll.aggregate(pipeline).toArray());

    // Verify that $expr returns the expected results when run in a find command.
    assert.sameMembers(metricsToCheck.expectedRes, coll.find({$expr: expr}).toArray());

    // Verify that $expr returns the correct number of results when evaluated inside a $project,
    // with optimizations inhibited. We expect the plan to be COLLSCAN.
    const pipelineWithProject = [
        {$_internalInhibitOptimization: {}},
        {$project: {result: {$cond: [expr, true, false]}}},
        {$_internalInhibitOptimization: {}},
        {$match: {result: true}}
    ];

    const expectedResultIds = metricsToCheck.expectedRes.map(doc => doc._id).sort();
    const actualResultIds =
        coll.aggregate(pipelineWithProject).toArray().map(doc => doc._id).sort();
    assert.eq(expectedResultIds, actualResultIds);

    let explain = coll.explain("executionStats").aggregate(pipelineWithProject);
    const isSharded = explain.hasOwnProperty("shards");

    if (isSharded) {
        const stages = getAggPlanStages(
            explain, "COLLSCAN", getEngine(explain) === "sbe" /* useQueryPlannerSection */);
        const numShards = Object.keys(explain.shards).length;
        assert.eq(
            stages.length,
            numShards,
            `Expected ${numShards} COLLSCAN stages (one per shard), but found ${stages.length}`);
    } else {
        assert(getAggPlanStage(
                   explain, "COLLSCAN", getEngine(explain) === "sbe" /* useQueryPlannerSection */),
               explain);
    }

    explain = assert.commandWorked(coll.explain("executionStats").aggregate(pipeline));
    verifyExplainOutput(explain, metricsToCheck, isSharded);

    explain = assert.commandWorked(coll.explain("executionStats").find({$expr: expr}).finish());
    verifyExplainOutput(explain, metricsToCheck, isSharded);
}

/**
 * Verifies that there are no rejected plans, and that the winning plan uses the expected index.
 */
function verifyExplainOutput(explain, metricsToCheck, isSharded) {
    assert(!hasRejectedPlans(explain), tojson(explain));

    const verifySingleExplain = (singleExplain) => {
        const stageName = metricsToCheck.hasOwnProperty("expectedIndex") ? "IXSCAN" : "COLLSCAN";
        const stage = getPlanStage(singleExplain, stageName);
        assert(stage, tojson(singleExplain));

        if (metricsToCheck.expectedIndex) {
            assert(stage.hasOwnProperty("keyPattern"), tojson(singleExplain));
            assert.docEq(metricsToCheck.expectedIndex, stage.keyPattern, tojson(singleExplain));
        }
    };

    if (isSharded) {
        const allWinningPlansValid = everyWinningPlan(explain, (winningPlan) => {
            verifySingleExplain(winningPlan);
            return true;
        });
        assert(allWinningPlansValid, tojson(explain));
    } else {
        verifySingleExplain(explain);
    }
}

/*
 * Supported by indexes
 */
const indexableTestCases = [
    // $in with all scalar values
    {
        expr: {$in: ["$category", ["clothing", "materials"]]},  // Strings
        expectedRes: [{_id: 0, category: "clothing"}, {_id: 2, category: "materials"}],
        expectedIndex: {category: 1}
    },
    {
        expr: {$in: ["$category", [1]]},  // Numerics
        expectedRes: [
            {_id: 21, category: 1},
            {_id: 22, category: 1.0},
            {_id: 23, category: NumberDecimal("1.00000000000000")},
            {_id: 24, category: NumberLong(1)}
        ],
        expectedIndex: {category: 1}
    },
    // $in with dotted paths
    {
        expr: {$in: ["$category.a", ["clothing", "electronics"]]},
        expectedRes: [{_id: 27, category: {a: "clothing"}}],
        expectedIndex: {"category.a": 1}
    },
    // $in with objects
    {
        expr: {$in: ["$category", [{}]]},  // Empty object
        expectedRes: [{_id: 5, category: {}}],
        expectedIndex: {"category": 1}
    },
    {
        expr: {$in: ["$category", [{a: 'clothing'}]]},  // Non-empty object
        expectedRes: [{_id: 27, category: {a: "clothing"}}],
        expectedIndex: {"category": 1}
    },
    {
        expr: {$in: ["$category", [{$toDouble: 1}]]},  // $toDouble
        expectedRes: [
            {_id: 21, category: 1},
            {_id: 22, category: 1.0},
            {_id: 23, category: NumberDecimal("1.00000000000000")},
            {_id: 24, category: NumberLong(1)}
        ],
        expectedIndex: {category: 1}
    },
    {
        expr: {$in: ["$category", [{$literal: {$toDouble: 1}}]]},  // $literal
        expectedRes: [{_id: 25, category: {$toDouble: 1}}],
        expectedIndex: {category: 1}
    }
];

/*
 * Unsupported by indexes
 */
const nonIndexableTestCases = [
    // $in with arrays
    {
        expr: {$in: ["$category", ["clothing", "materials", ["clothing", "electronics"]]]},
        expectedRes: [
            {_id: 0, category: "clothing"},
            {_id: 2, category: "materials"},
            {_id: 10, category: ["clothing", "electronics"]}
        ]
    },
    {
        expr: {$in: ["$category", [[]]]},  // Nested empty array
        expectedRes: [{_id: 6, category: []}]
    },
    {
        expr: {$in: ["$category.a", [[]]]},  // Dotted path, empty array
        expectedRes: [
            {_id: 6, category: []},
            {_id: 7, category: [[]]},
            {_id: 8, category: [null]},
            {_id: 9, category: [[], [""]]},
            {_id: 10, category: ["clothing", "electronics"]},
            {_id: 11, category: ["materials", "clothing", "electronics"]},
            {_id: 12, category: [["clothing"], ["electronics"]]},
            {_id: 15, category: [{}]},
            {_id: 16, category: [["clothing", "electronics"]]},
            {_id: 17, category: [[["clothing", "materials", "electronics"]]]},
            {_id: 18, category: [[["clothing", "electronics"]]]},
            {_id: 19, category: [[[null]]]}
        ]
    },
    // $in with regex
    {expr: {$in: ["$category", [/clothing/]]}, expectedRes: [{_id: 13, category: /clothing/}]},
    // $in with null
    {expr: {$in: ["$category", [null]]}, expectedRes: [{_id: 4, category: null}]},
    // $in with $getField in lhs
    {
        expr: {$in: [{$getField: "category.a"}, ["clothing", "electronics"]]},  // $getField
        expectedRes: [{_id: 26, "category.a": "clothing"}]
    }
];

indexableTestCases.forEach(({expr, expectedRes, expectedIndex}) => {
    confirmExpectedExprExecution(expr, {expectedRes, expectedIndex});
});

nonIndexableTestCases.forEach(({expr, expectedRes}) => {
    confirmExpectedExprExecution(expr, {expectedRes});
});
