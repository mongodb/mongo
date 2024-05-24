// Test that an index can be used to accelerate count commands, as well as the $count agg
// stage.
//
// The collection cannot be sharded, since the requirement to SHARD_FILTER precludes the planner
// from generating a COUNT_SCAN plan. Further, we do not allow stepdowns, since the code responsible
// for retrying on interrupt is not prepared to handle aggregation explain.
// @tags: [
//   assumes_unsharded_collection,
//   does_not_support_stepdowns,
//   # Test may fail with "index already exists".
//   assumes_no_implicit_index_creation,
//   # Explain for the aggregate command cannot run within a multi-document transaction.
//   does_not_support_transactions,
// ]
import {getPlanStage, getSingleNodeExplain} from "jstests/libs/analyze_plan.js";

const coll = db.index_count;
coll.drop();

assert.commandWorked(coll.insert([
    {a: 1},
    {a: 1, b: 1},
    {a: 2},
    {a: 3},
    {a: null},
    {a: [-1, 0]},
    {a: [4, -3, 5]},
    {},
    {a: {b: 4}},
    {a: []},
    {a: [[], {}]},
    {a: {}},
    {a: true},
    {a: false},
    {a: ""}
]));

// Retrieve the query plain from explain, whose shape varies depending on the query and the
// engines used (classic/sbe).
const getQueryPlan = function(explain) {
    if (explain.stages) {
        explain = explain.stages[0].$cursor;
    }
    let winningPlan = explain.queryPlanner.winningPlan;
    return winningPlan.queryPlan ? [winningPlan.queryPlan, winningPlan.slotBasedPlan]
                                 : [winningPlan, null];
};

// Verify that this query uses a COUNT_SCAN.
const runAndVerify = function(expectedCount, pipeline, stage, sbePlanStage) {
    assert.eq(expectedCount, coll.aggregate(pipeline).next().count);
    let explain = getSingleNodeExplain(coll.explain().aggregate(pipeline));
    const [queryPlan, sbePlan] = getQueryPlan(explain);
    let countScan = getPlanStage(queryPlan, stage);
    assert.neq(null, countScan, explain);
    if (sbePlan) {
        assert.eq(true, sbePlan.stages.includes(sbePlanStage), sbePlan);
    }
};

// Verify that this query uses a COUNT_SCAN.
const runAndVerifyCountScan = function(expectedCount, pipeline) {
    runAndVerify(expectedCount, pipeline, "COUNT_SCAN", "ixseek");
};

// Verify that this query uses an IXSCAN.
const runAndVerifyIxscan = function(expectedCount, pipeline) {
    runAndVerify(expectedCount, pipeline, "IXSCAN", "ixscan_generic");
};

const runTest = function(indexPattern, indexOption = {}) {
    assert.commandWorked(coll.createIndex(indexPattern, indexOption));

    assert.eq(5, coll.count({a: {$gt: 0}}));
    assert.eq(5, coll.find({a: {$gt: 0}}).itcount());

    runAndVerifyCountScan(2, [{$match: {a: 1}}, {$count: "count"}]);
    // Run more times to ensure the query is cached.
    runAndVerifyCountScan(2, [{$match: {a: 1}}, {$count: "count"}]);
    runAndVerifyCountScan(2, [{$match: {a: 1}}, {$count: "count"}]);
    // Make sure query is parameterized correctly for count scan index keys.
    runAndVerifyCountScan(1, [{$match: {a: 2}}, {$count: "count"}]);
    if (indexPattern.b) {
        runAndVerifyCountScan(1, [{$match: {a: 1, b: 1}}, {$count: "count"}]);
    }
    runAndVerifyCountScan(2, [{$match: {a: {}}}, {$count: "count"}]);
    runAndVerifyCountScan(3, [{$match: {a: {$gt: 1}}}, {$count: "count"}]);
    // Add a $project stage between $match and $count to avoid pushdown.
    runAndVerifyCountScan(2, [{$match: {a: 1}}, {$project: {_id: 0, a: 0}}, {$count: "count"}]);
    if (indexPattern.a) {
        runAndVerifyCountScan(15, [{$sort: {a: 1}}, {$count: "count"}]);
        runAndVerifyCountScan(15, [{$sort: {a: -1}}, {$count: "count"}]);
        runAndVerifyCountScan(15, [{$sort: {a: -1}}, {$group: {_id: null, count: {$sum: 1}}}]);
    }

    assert.commandWorked(coll.dropIndex(indexPattern));
};

runTest({a: 1});
runTest({"$**": 1});
runTest({"$**": -1, b: -1}, {wildcardProjection: {b: 0}});

// Delete array values to get non-multikey indexes.
assert.commandWorked(coll.deleteMany({a: {$type: "array"}}));

(function runParametrizationTest(indexPattern, indexOption = {}) {
    assert.commandWorked(coll.createIndex(indexPattern, indexOption));

    // Make sure we generate two separate cache entries for queries with values that cannot be
    // auto-parametrized, and hence use the right plan. In the case below, the first query generates
    // index bounds [MinKey, MaxKey], and hence is eligible for a COUNT_SCAN, whereas query 2
    // genereates bounds [MinKey, false) [true, MaxKey], which cannot use a COUNT_SCAN.
    runAndVerifyCountScan(11, [{$match: {a: {$not: {$lt: false}}}}, {$count: "count"}]);
    // Repeat to cache plan.
    runAndVerifyCountScan(11, [{$match: {a: {$not: {$lt: false}}}}, {$count: "count"}]);
    runAndVerifyCountScan(11, [{$match: {a: {$not: {$lt: false}}}}, {$count: "count"}]);
    // Verify that a query matching the shape that should not use a COUNT_SCAN plan doesn't match
    // the index entry.
    runAndVerifyIxscan(10, [{$match: {a: {$not: {$lt: true}}}}, {$count: "count"}]);

    // Similar to the above, the query below generates index bounds [MinKey, MaxKey], whereas the
    // query after it generates bounds [MinKey, -inf), [3, MaxKey], which is ineligible for a
    // COUNT_SCAN.
    runAndVerifyCountScan(11, [{$match: {a: {$not: {$lt: ""}}}}, {$count: "count"}]);
    // Repeat to cache plan.
    runAndVerifyCountScan(11, [{$match: {a: {$not: {$lt: ""}}}}, {$count: "count"}]);
    runAndVerifyCountScan(11, [{$match: {a: {$not: {$lt: ""}}}}, {$count: "count"}]);
    // Verify that a query matching the shape that should not use a COUNT_SCAN plan doesn't match
    // the index entry.
    runAndVerifyIxscan(8, [{$match: {a: {$not: {$lt: 3}}}}, {$count: "count"}]);

    assert.commandWorked(coll.dropIndex(indexPattern));
})({a: 1});
