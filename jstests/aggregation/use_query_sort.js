// Tests that an aggregation with a $sort near the front of the pipeline can sometimes use the query
// system to provide the sort.
//
// Relies on the ability to push leading $sorts down to the query system, so cannot wrap pipelines
// in $facet stages:
// @tags: [
//   do_not_wrap_aggregations_in_facets,
// ]
import {
    hasRejectedPlans,
    isQueryPlan,
    planHasStage,
} from "jstests/libs/analyze_plan.js";

const coll = db.use_query_sort;
coll.drop();

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 100; ++i) {
    bulk.insert({_id: i, x: "string", a: -i, y: i % 2});
}
assert.commandWorked(bulk.execute());

function assertHasNonBlockingQuerySort(pipeline, expectRejectedPlans) {
    const explainOutput = coll.explain().aggregate(pipeline);
    assert(isQueryPlan(explainOutput), explainOutput);
    assert(!planHasStage(db, explainOutput, "SORT"), explainOutput);
    assert(planHasStage(db, explainOutput, "IXSCAN"), explainOutput);
    assert.eq(expectRejectedPlans, hasRejectedPlans(explainOutput), explainOutput);
    return explainOutput;
}

function assertHasBlockingQuerySort(pipeline, expectRejectedPlans) {
    const explainOutput = coll.explain().aggregate(pipeline);
    assert(isQueryPlan(explainOutput), explainOutput);
    assert(planHasStage(db, explainOutput, "SORT"), explainOutput);
    assert.eq(expectRejectedPlans, hasRejectedPlans(explainOutput), explainOutput);
}

// Test that a sort on _id can use the query system to provide the sort. Since the sort and match
// are both on the _id field, we don't expect there to be any rejected plans.
assertHasNonBlockingQuerySort([{$sort: {_id: -1}}], false);
assertHasNonBlockingQuerySort([{$sort: {_id: 1}}], false);
assertHasNonBlockingQuerySort([{$match: {_id: {$gte: 50}}}, {$sort: {_id: 1}}], false);
assertHasNonBlockingQuerySort([{$match: {_id: {$gte: 50}}}, {$sort: {_id: -1}}], false);

// Test that a sort on a field not in any index will use a SORT stage in the query layer. Since
// there is no index to support the sort, we don't expect any rejected plans.
assertHasBlockingQuerySort([{$sort: {x: -1}}], false);
assertHasBlockingQuerySort([{$sort: {x: 1}}], false);
assertHasBlockingQuerySort([{$match: {_id: {$gte: 50}}}, {$sort: {x: 1}}], false);

assert.commandWorked(coll.createIndex({x: 1, y: -1}));

// Since there is an index to support these sorts, we expect the system to choose a non-blocking
// sort. The only indexed plan is an index-provided sort, so we don't expect any rejected plans.
assertHasNonBlockingQuerySort([{$sort: {x: 1, y: -1}}], false);
assertHasNonBlockingQuerySort([{$sort: {x: 1}}], false);

// These sorts cannot be provided by an index, but it still should get pushed down to the query
// layer. The only plan is a COLLSCAN followed by a blocking sort, so we don't expect any rejected
// plans.
assertHasBlockingQuerySort([{$sort: {y: 1}}], false);
assertHasBlockingQuerySort([{$sort: {x: 1, y: 1}}], false);

// In this case, there are two possible plans: an _id index scan with a blocking SORT, or an
// index-provided sort by scanning the {x: 1, y: -1} index. Since the _id predicate is more
// selective, we expect the blocking SORT plan to win and there to be a rejected plan.
assertHasBlockingQuerySort([{$match: {_id: {$gte: 90}}}, {$sort: {x: 1}}], true);
// A query of the same shape will use a non-blocking plan if the predicate is not selective.
assertHasNonBlockingQuerySort([{$match: {_id: {$gte: 0}}}, {$sort: {x: 1}}], true);

// Verify that meta-sort on "textScore" can be pushed down into the query layer.
assert.commandWorked(coll.createIndex({x: "text"}));
assertHasBlockingQuerySort(
    [{$match: {$text: {$search: "test"}}}, {$sort: {key: {$meta: "textScore"}}}], false);

// Verify that meta-sort on "randVal" can be pushed into the query layer. Although "randVal" $meta
// sort is currently a supported way to randomize the order of the data, it shouldn't preclude
// pushdown of the sort into the plan stage layer.
assertHasBlockingQuerySort([{$sort: {key: {$meta: "randVal"}}}], false);
