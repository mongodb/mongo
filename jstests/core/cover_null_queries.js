/**
 * Test to verify that null queries can be fully covered by an index.
 * @tags: [
 *   assumes_unsharded_collection,
 *   requires_non_retryable_writes,
 *   requires_fcv_62,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq().
load("jstests/libs/analyze_plan.js");         // For getAggPlanStages() and getPlanStages().

const coll = db.cover_null_queries;
coll.drop();

assert.commandWorked(coll.insertMany([
    {_id: 1, a: 1, b: 1},
    {_id: 2, a: 1, b: null},
    {_id: 3, a: null, b: 1},
    {_id: 4, a: null, b: null},
    {_id: 5, a: 2},
    {_id: 6, b: 2},
    {_id: 7},
]));

/**
 * Validates that the explain() of command 'cmdObj' has the stages in 'expectedStages'.
 *
 * The field 'expectedStages' should be specified as an object with keys matching the stages
 * expected in the explain output and values indicating how many times each stage should be expected
 * to appear in the output (useful to verify if there are two COUNT_SCANS or that there are 0
 * IX_SCANS, for example).
 *
 * The field 'isAgg' is a boolean indicating whether or not the command is an aggregation.
 */
function validateStages({cmdObj, expectedStages, isAgg}) {
    const explainObj = assert.commandWorked(coll.runCommand({explain: cmdObj}));
    for (const [expectedStage, count] of Object.entries(expectedStages)) {
        const planStages = isAgg
            ? getAggPlanStages(explainObj, expectedStage, /* useQueryPlannerSection */ true)
            : getPlanStages(explainObj, expectedStage);
        assert.eq(planStages.length, count, {foundStages: planStages, explain: explainObj});
        if (count > 0) {
            for (const planStage of planStages) {
                assert.eq(planStage.stage, expectedStage, planStage);
            }
        }
    }
}

/**
 * Runs find command with the given 'filter' and 'projection' and validates that the output returned
 * matches 'expectedOutput'. Also runs explain() command on the same find command and validates that
 * all the 'expectedStages' are present in the plan returned.
 */
function validateFindCmdOutputAndPlan({filter, projection, expectedStages, expectedOutput}) {
    const cmdObj = {find: coll.getName(), filter: filter, projection: projection};
    if (expectedOutput) {
        const res = assert.commandWorked(coll.runCommand(cmdObj));
        const ouputArray = new DBCommandCursor(coll.getDB(), res).toArray();
        assert(arrayEq(expectedOutput, ouputArray), ouputArray);
    }
    validateStages({cmdObj, expectedStages});
}

/**
 * Runs count command with the 'filter' and validates that the output returned matches
 * 'expectedOutput'. Also runs explain() command and validates that all the 'expectedStages'
 * are present in the plan returned.
 */
function validateSimpleCountCmdOutputAndPlan({filter, expectedStages, expectedCount}) {
    const cmdObj = {count: coll.getName(), query: filter};
    const res = assert.commandWorked(coll.runCommand(cmdObj));
    assert.eq(res.n, expectedCount);
    validateStages({cmdObj, expectedStages});
}

/**
 * Runs an aggregation with a $count stage with the 'filter' applied to the $match stage and
 * validates that the count returned matches 'expectedCount'. Also runs explain() command on the
 * and validates that all the 'expectedStages' are present in the plan returned.
 */
function validateCountAggCmdOutputAndPlan({filter, expectedStages, expectedCount, pipeline}) {
    const cmdObj = {
        aggregate: coll.getName(),
        pipeline: pipeline || [{$match: filter}, {$count: "count"}],
        cursor: {},
    };
    const cmdRes = assert.commandWorked(coll.runCommand(cmdObj));
    const countRes = cmdRes.cursor.firstBatch;
    assert.eq(countRes.length, 1, cmdRes);
    assert.eq(countRes[0].count, expectedCount, countRes);
    validateStages({cmdObj, expectedStages, isAgg: true});
}

assert.commandWorked(coll.createIndex({a: 1, _id: 1}));

// Verify count({a: null}) can be covered by an index. In the simplest case we can use two count
// scans joined by an OR to evaluate it.
validateSimpleCountCmdOutputAndPlan({
    filter: {a: null},
    expectedCount: 4,
    expectedStages: {"OR": 1, "COUNT_SCAN": 2, "IXSCAN": 0, "FETCH": 0},
});

// Verify $count stage in aggregation matching {a: null} yields the same plan.
validateCountAggCmdOutputAndPlan({
    filter: {a: null},
    expectedCount: 4,
    expectedStages: {"OR": 1, "COUNT_SCAN": 2, "IXSCAN": 0, "FETCH": 0},
});

// Verify find({a: null}, {_id: 1}) can be covered by an index.
validateFindCmdOutputAndPlan({
    filter: {a: null},
    projection: {_id: 1},
    expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});

// Verify that the above queries can be covered by an index when the predicate is a $in. These are
// not supported by a COUNT_SCAN because they are not the strict null equality predicate.
validateCountAggCmdOutputAndPlan({
    filter: {a: {$in: [null, 2]}},
    expectedCount: 5,
    expectedStages: {"IXSCAN": 1, "FETCH": 0},
});
validateSimpleCountCmdOutputAndPlan({
    filter: {a: {$in: [null, 2]}},
    expectedCount: 5,
    expectedStages: {"IXSCAN": 1, "FETCH": 0},
});
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [null, 2]}},
    projection: {_id: 1},
    expectedOutput: [{_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}, {_id: 7}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});

// Same as above, but using a different ordering in the $in clause.
validateSimpleCountCmdOutputAndPlan({
    filter: {a: {$in: [2, null]}},
    expectedCount: 5,
    expectedStages: {"IXSCAN": 1, "FETCH": 0},
});
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [2, null]}},
    projection: {_id: 1},
    expectedOutput: [{_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}, {_id: 7}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});

// Same as above, but special case for null and empty array predicate.
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}},
    projection: {_id: 1},
    expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});
validateSimpleCountCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}},
    expectedCount: 4,
    expectedStages: {"IXSCAN": 1, "FETCH": 0},
});

// Verify that a more complex projection that only relies on the _id field does not need a FETCH.
validateFindCmdOutputAndPlan({
    filter: {a: null},
    projection: {_id: 1, incr_id: {$add: [1, "$_id"]}},
    expectedOutput:
        [{_id: 3, incr_id: 4}, {_id: 4, incr_id: 5}, {_id: 6, incr_id: 7}, {_id: 7, incr_id: 8}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_DEFAULT": 1},
});

// Verify that a more complex projection that computes a new field based on _id but excludes the _id
// field does not require a fetch stage.
validateFindCmdOutputAndPlan({
    filter: {a: null},
    projection: {_id: 0, incr_id: {$add: [1, "$_id"]}},
    expectedOutput: [{incr_id: 4}, {incr_id: 5}, {incr_id: 7}, {incr_id: 8}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_DEFAULT": 1},
});

// Verify that a more complex projection that relies on any non-_id field does need a FETCH.
validateFindCmdOutputAndPlan({
    filter: {a: null},
    projection: {_id: 1, incr_id: {$add: ["$a", "$_id"]}},
    expectedOutput: [
        {_id: 3, incr_id: null},
        {_id: 4, incr_id: null},
        {_id: 6, incr_id: null},
        {_id: 7, incr_id: null}
    ],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_DEFAULT": 1},
});

// Verify that an exclusion projection does need a FETCH.
validateFindCmdOutputAndPlan({
    filter: {a: null},
    projection: {a: 0, b: 0},
    expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
});

// Verify find({a: null}, {_id: 1, b: 1}) is not covered by an index so we still have a FETCH stage.
validateFindCmdOutputAndPlan({
    filter: {a: null},
    projection: {_id: 1, b: 1},
    expectedOutput: [{_id: 3, b: 1}, {_id: 4, b: null}, {_id: 6, b: 2}, {_id: 7}],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
});
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [null, 2]}},
    projection: {_id: 1, b: 1},
    expectedOutput: [{_id: 3, b: 1}, {_id: 4, b: null}, {_id: 5}, {_id: 6, b: 2}, {_id: 7}],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
});

// Verify find({a: null}, {a: 1}) still has a FETCH stage because the index alone cannot determine
// if the value of field a is null, undefined, or missing.
validateFindCmdOutputAndPlan({
    filter: {a: null},
    projection: {a: 1},
    expectedOutput: [{_id: 3, a: null}, {_id: 4, a: null}, {_id: 6}, {_id: 7}],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
});
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [null, 2]}},
    projection: {a: 1},
    expectedOutput: [{_id: 3, a: null}, {_id: 4, a: null}, {_id: 5, a: 2}, {_id: 6}, {_id: 7}],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
});

// For exclusion projects we always need a FETCH stage.
validateFindCmdOutputAndPlan({
    filter: {a: null},
    projection: {a: 1, _id: 0},
    expectedOutput: [{a: null}, {a: null}, {}, {}],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
});

// Verify that if the index is multikey, this optimization cannot be applied when just querying for
// null values, as the index alone cannot differentiate between null and [].
assert.commandWorked(coll.insertOne({_id: 8, a: []}));
assert.commandWorked(coll.insertOne({_id: 9, a: [[]]}));
assert.commandWorked(coll.insertOne({_id: 10, a: [null, []]}));

validateSimpleCountCmdOutputAndPlan({
    filter: {a: null},
    expectedCount: 5,
    expectedStages: {"FETCH": 1, "IXSCAN": 1, "OR": 0, "COUNT_SCAN": 0}
});
validateCountAggCmdOutputAndPlan({
    filter: {a: null},
    expectedCount: 5,
    expectedStages: {"FETCH": 1, "IXSCAN": 1, "OR": 0, "COUNT_SCAN": 0},
});
validateFindCmdOutputAndPlan({
    filter: {a: null},
    projection: {_id: 1},
    expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 10}],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
});
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [null, 2]}},
    projection: {_id: 1},
    expectedOutput: [{_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}, {_id: 7}, {_id: 10}],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
});
validateSimpleCountCmdOutputAndPlan({
    filter: {a: null, _id: 3},
    expectedCount: 1,
    expectedStages: {"FETCH": 1, "IXSCAN": 1, "OR": 0, "COUNT_SCAN": 0}
});
validateCountAggCmdOutputAndPlan({
    filter: {a: null, _id: 3},
    expectedCount: 1,
    expectedStages: {"FETCH": 1, "IXSCAN": 1, "OR": 0, "COUNT_SCAN": 0},
});
validateFindCmdOutputAndPlan({
    filter: {a: null, _id: 3},
    projection: {_id: 1},
    expectedOutput: [{_id: 3}],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
});

// Verify that if the index is multikey and the query searches for null and empty array values, then
// the find does not require a FETCH stage, and a count can replace the IXSCAN with three
// COUNT_SCANS joined by an OR.
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}},
    projection: {_id: 1},
    expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 8}, {_id: 9}, {_id: 10}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});
validateCountAggCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}},
    expectedCount: 7,
    expectedStages: {"FETCH": 0, "IXSCAN": 0, "OR": 1, "COUNT_SCAN": 3},
});
validateSimpleCountCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}},
    expectedCount: 7,
    expectedStages: {"OR": 1, "COUNT_SCAN": 3, "IXSCAN": 0, "FETCH": 0},
});

// Same as above, but using a different ordering in the $in clause.
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [[], null]}},
    projection: {_id: 1},
    expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 8}, {_id: 9}, {_id: 10}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});
validateCountAggCmdOutputAndPlan({
    filter: {a: {$in: [[], null]}},
    expectedCount: 7,
    expectedStages: {"FETCH": 0, "IXSCAN": 0, "OR": 1, "COUNT_SCAN": 3},
});

// Verify that the same optimization is supported when using $or syntax.
validateFindCmdOutputAndPlan({
    filter: {$or: [{a: null}, {a: []}]},
    projection: {_id: 1},
    expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 8}, {_id: 9}, {_id: 10}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});
validateCountAggCmdOutputAndPlan({
    filter: {$or: [{a: null}, {a: []}]},
    expectedCount: 7,
    expectedStages: {"FETCH": 0, "IXSCAN": 0, "OR": 1, "COUNT_SCAN": 3},
});
validateSimpleCountCmdOutputAndPlan({
    filter: {$or: [{a: null}, {a: []}]},
    expectedCount: 7,
    expectedStages: {"OR": 1, "COUNT_SCAN": 3, "IXSCAN": 0, "FETCH": 0},
});

// Same as above, but using a different ordering in the $in clauses.
validateFindCmdOutputAndPlan({
    filter: {$or: [{a: []}, {a: null}]},
    projection: {_id: 1},
    expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 8}, {_id: 9}, {_id: 10}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});
validateCountAggCmdOutputAndPlan({
    filter: {$or: [{a: []}, {a: null}]},
    expectedCount: 7,
    expectedStages: {"FETCH": 0, "IXSCAN": 0, "OR": 1, "COUNT_SCAN": 3},
});

// Verify that if the index is multikey and the query searches for null, empty array, and other
// values, then it does not require a FETCH, but we cannot replace the IXSCAN with COUNTs.
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [null, [], 1]}},
    projection: {_id: 1},
    expectedOutput:
        [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 8}, {_id: 9}, {_id: 10}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});
validateCountAggCmdOutputAndPlan({
    filter: {a: {$in: [null, [], 1]}},
    expectedCount: 9,
    expectedStages: {"FETCH": 0, "IXSCAN": 1, "OR": 0, "COUNT_SCAN": 0},
});
validateSimpleCountCmdOutputAndPlan({
    filter: {a: {$in: [null, [], 1]}},
    expectedCount: 9,
    expectedStages: {"OR": 0, "COUNT_SCAN": 0, "IXSCAN": 1, "FETCH": 0},
});

// Verify that if the index is multikey and the query searches for null and empty array values, we
// still fetch when projecting a field other than _id.
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}},
    projection: {_id: 1, a: 1},
    expectedOutput: [
        {_id: 3, a: null},
        {_id: 4, a: null},
        {_id: 6},
        {_id: 7},
        {_id: 8, a: []},
        {_id: 9, a: [[]]},
        {_id: 10, a: [[], null]}
    ],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
});
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}},
    projection: {_id: 1, b: 1},
    expectedOutput: [
        {_id: 3, b: 1},
        {_id: 4, b: null},
        {_id: 6, b: 2},
        {_id: 7},
        {_id: 8},
        {_id: 9},
        {_id: 10}
    ],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
});

assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({a: 1, b: 1, _id: 1}));

// Verify that if the index is multikey and compound and the query matches null and empty array
// values, then it does not require a FETCH stage and can replace the IXSCAN with COUNTs.
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}},
    projection: {_id: 1},
    expectedOutput: [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 8}, {_id: 9}, {_id: 10}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});
validateCountAggCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}},
    expectedCount: 7,
    expectedStages: {"FETCH": 0, "IXSCAN": 0, "OR": 1, "COUNT_SCAN": 3},
});
validateSimpleCountCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}},
    expectedCount: 7,
    expectedStages: {"OR": 1, "COUNT_SCAN": 3, "IXSCAN": 0, "FETCH": 0},
});

// Verify that if the index is multikey and compound and the query has an added compound predicate,
// then it does not require a FETCH and can replace the IXSCAN with COUNTs.
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}, b: {$eq: 2}},
    projection: {_id: 1},
    expectedOutput: [{_id: 6}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});
validateCountAggCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}, b: {$eq: 2}},
    expectedCount: 1,
    expectedStages: {"FETCH": 0, "IXSCAN": 0, "OR": 1, "COUNT_SCAN": 3},
});
validateSimpleCountCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}, b: {$eq: 2}},
    expectedCount: 1,
    expectedStages: {"OR": 1, "COUNT_SCAN": 3, "IXSCAN": 0, "FETCH": 0},
});
validateFindCmdOutputAndPlan({
    filter: {a: 1, b: {$in: [null, []]}},
    projection: {_id: 1},
    expectedOutput: [{_id: 2}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});
validateCountAggCmdOutputAndPlan({
    filter: {a: 1, b: {$in: [null, []]}},
    expectedCount: 1,
    expectedStages: {"FETCH": 0, "IXSCAN": 0, "OR": 1, "COUNT_SCAN": 3},
});
validateSimpleCountCmdOutputAndPlan({
    filter: {a: 1, b: {$in: [null, []]}},
    expectedCount: 1,
    expectedStages: {"OR": 1, "COUNT_SCAN": 3, "IXSCAN": 0, "FETCH": 0},
});

// Verify if the index is multikey and compound and the query searches for null, empty array, and
// other values, then it does not require a FETCH, but we cannot replace the IXSCAN with COUNTs.
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [null, [], 1]}, b: {$eq: 2}},
    projection: {_id: 1},
    expectedOutput: [{_id: 6}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});
validateCountAggCmdOutputAndPlan({
    filter: {a: {$in: [null, [], 1]}, b: {$eq: 2}},
    expectedCount: 1,
    expectedStages: {"FETCH": 0, "IXSCAN": 1, "OR": 0, "COUNT_SCAN": 0},
});
validateSimpleCountCmdOutputAndPlan({
    filter: {a: {$in: [null, [], 1]}, b: {$eq: 2}},
    expectedCount: 1,
    expectedStages: {"OR": 0, "COUNT_SCAN": 0, "IXSCAN": 1, "FETCH": 0},
});

// Same as above, but using a different ordering in the $in clauses.
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [null, 1, []]}, b: {$eq: 2}},
    projection: {_id: 1},
    expectedOutput: [{_id: 6}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});
validateCountAggCmdOutputAndPlan({
    filter: {a: {$in: [null, 1, []]}, b: {$eq: 2}},
    expectedCount: 1,
    expectedStages: {"FETCH": 0, "IXSCAN": 1, "OR": 0, "COUNT_SCAN": 0},
});

// Verify if the index is multikey and compound and the query searches for null, empty array, and
// other values for multiple fields, then it does not require a FETCH but cannot replace the IXSCAN
// with COUNTs because there are multiple null intervals or because the intervals are complex.
validateCountAggCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}, b: {$in: [null, []]}},
    expectedCount: 5,
    expectedStages: {"FETCH": 0, "IXSCAN": 1, "OR": 0, "COUNT_SCAN": 0},
});
validateCountAggCmdOutputAndPlan({
    filter: {a: {$in: [null, [], 1]}, b: {$in: [null, []]}},
    expectedCount: 6,
    expectedStages: {"FETCH": 0, "IXSCAN": 1, "OR": 0, "COUNT_SCAN": 0},
});
validateCountAggCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}, b: {$in: [null, [], 4]}},
    expectedCount: 5,
    expectedStages: {"FETCH": 0, "IXSCAN": 1, "OR": 0, "COUNT_SCAN": 0},
});
validateCountAggCmdOutputAndPlan({
    filter: {a: {$in: [null, [], 1]}, b: {$in: [null, [], 4]}},
    expectedCount: 6,
    expectedStages: {"FETCH": 0, "IXSCAN": 1, "OR": 0, "COUNT_SCAN": 0},
});

// Verify if the index is multikey and compound, the query predicate must match null and empty array
// values for all queried fields.
validateCountAggCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}, b: null},
    expectedCount: 5,
    expectedStages: {"FETCH": 1, "IXSCAN": 1, "OR": 0, "COUNT_SCAN": 0},
});

assert.commandWorked(coll.deleteMany({_id: {$in: [8, 9, 10]}}));

// Same as above but when the index is not multikey. In this case, we can cover the query, but we
// cannot do the COUNT_SCAN optimization because there are multiple null intervals.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({a: 1, b: 1, _id: 1}));
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}, b: null},
    projection: {_id: 1},
    expectedOutput: [{_id: 4}, {_id: 7}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});
validateSimpleCountCmdOutputAndPlan({
    filter: {a: {$in: [null, []]}, b: null},
    expectedCount: 2,
    expectedStages: {"IXSCAN": 1, "FETCH": 0},
});

// Test case when query is fully covered but we still need to fetch to correctly project a field.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({a: 1, _id: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1, _id: 1}));
validateFindCmdOutputAndPlan({
    filter: {a: null},
    projection: {_id: 1, b: 1},
    expectedOutput: [
        {_id: 3, b: 1},
        {_id: 4, b: null},
        {_id: 6, b: 2},
        {_id: 7},
    ],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
});

// Test case when query is fully covered but predicate is not a single interval.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({a: 1, b: 1, _id: 1}));
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [1, 2, 3]}, b: null},
    projection: {_id: 1},
    expectedOutput: [{_id: 2}, {_id: 5}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});
// Note that we can't use a COUNT_SCAN here because we have a complex interval.
validateSimpleCountCmdOutputAndPlan({
    filter: {a: {$in: [1, 2, 3]}, b: null},
    expectedCount: 2,
    expectedStages: {"IXSCAN": 1, "FETCH": 0},
});
validateCountAggCmdOutputAndPlan({
    filter: {a: {$in: [1, 2, 3]}, b: null},
    expectedCount: 2,
    expectedStages: {"IXSCAN": 1, "FETCH": 0},
});

validateFindCmdOutputAndPlan({
    filter: {a: 1, b: null},
    projection: {_id: 1},
    expectedOutput: [{_id: 2}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});
validateSimpleCountCmdOutputAndPlan({
    filter: {a: 1, b: null},
    expectedCount: 1,
    expectedStages: {"OR": 1, "COUNT_SCAN": 2, "FETCH": 0},
});
validateCountAggCmdOutputAndPlan({
    filter: {a: 1, b: null},
    expectedCount: 1,
    expectedStages: {"OR": 1, "COUNT_SCAN": 2, "FETCH": 0},
});

validateFindCmdOutputAndPlan({
    filter: {a: 1, b: {$in: [null, 1]}},
    projection: {_id: 1},
    expectedOutput: [{_id: 1}, {_id: 2}],
    expectedStages: {"IXSCAN": 1, "FETCH": 0, "PROJECTION_COVERED": 1},
});
validateSimpleCountCmdOutputAndPlan({
    filter: {a: 1, b: {$in: [null, 1]}},
    expectedCount: 2,
    expectedStages: {"IXSCAN": 1, "FETCH": 0},
});
validateCountAggCmdOutputAndPlan({
    filter: {a: 1, b: {$in: [null, 1]}},
    expectedCount: 2,
    expectedStages: {"IXSCAN": 1, "FETCH": 0},
});

// Test case when counting nulls where documents are sorted in the opposite direction as the index.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({a: -1, b: -1}));
validateCountAggCmdOutputAndPlan({
    expectedCount: 2,
    expectedStages: {"OR": 1, "COUNT_SCAN": 2, "FETCH": 0},
    pipeline: /* Sort by field a in the opposite direction of the index. */
        [{$match: {a: null, b: {$gt: 0}}}, {$sort: {a: 1}}, {$count: "count"}],
});

// Test case when query is fully covered, predicate is not a single interval, and the index does not
// include the _id field. A find projection in this case will not be covered, but any count should
// be covered.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({a: 1, b: 1}));
validateFindCmdOutputAndPlan({
    filter: {a: {$in: [1, 2, 3]}, b: null},
    projection: {_id: 1},
    expectedOutput: [{_id: 2}, {_id: 5}],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
});
// Note that we can't use a COUNT_SCAN here because we have a complex interval.
validateSimpleCountCmdOutputAndPlan({
    filter: {a: {$in: [1, 2, 3]}, b: null},
    expectedCount: 2,
    expectedStages: {"IXSCAN": 1, "FETCH": 0},
});
validateCountAggCmdOutputAndPlan({
    filter: {a: {$in: [1, 2, 3]}, b: null},
    expectedCount: 2,
    expectedStages: {"IXSCAN": 1, "FETCH": 0},
});

// Test index intersection plan.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({a: 1, _id: 1}));
assert.commandWorked(coll.createIndex({b: 1, _id: 1}));
validateFindCmdOutputAndPlan({
    filter: {a: null, b: null},
    projection: {_id: 1},
    expectedOutput: [{_id: 4}, {_id: 7}],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
});
validateFindCmdOutputAndPlan({
    filter: {a: null, b: 1},
    projection: {_id: 1},
    expectedOutput: [{_id: 3}],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_SIMPLE": 1},
});

// Verify the case where id field is accessed along a dotted path.
// We still need a FETCH in this case because the index cannot differentiate between null and
// missing values within _id, e.g. {_id: {x: null}} vs. {_id: {}} would both be returned as
// {_id: {x: null}} by the index.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.deleteMany({}));
assert.commandWorked(coll.createIndex({a: 1, "_id.x": 1}));
assert.commandWorked(coll.insertMany([
    {a: null, _id: {x: 1}},
    {a: null, _id: {x: 1, y: 1}},
    {a: null, _id: {y: 1}},
    {_id: {x: 1, y: 2}},
    {a: "not null", _id: {x: 3}},
]));
validateFindCmdOutputAndPlan({
    filter: {a: null},
    projection: {"_id.x": 1},
    expectedOutput: [{_id: {x: 1}}, {_id: {x: 1}}, {_id: {}}, {_id: {x: 1}}],
    expectedStages: {"IXSCAN": 1, "FETCH": 1, "PROJECTION_DEFAULT": 1},
});
validateSimpleCountCmdOutputAndPlan({
    filter: {a: null},
    expectedCount: 4,
    expectedStages: {"OR": 1, "COUNT_SCAN": 2, "IXSCAN": 0, "FETCH": 0},
});
validateCountAggCmdOutputAndPlan({
    filter: {a: null},
    expectedCount: 4,
    expectedStages: {"OR": 1, "COUNT_SCAN": 2, "IXSCAN": 0, "FETCH": 0},
});
})();
