/**
 * Tests that the $limit stage is pushed before $lookup stages, except when there is an $unwind.
 * This will be run against a sharded cluster, which invalidates the disablePipelineOptimization
 * failpoints that the standalone 'lookup_with_limit' tests use.
 *
 * For an unsharded collection, the result of 'explain()' is matched against the expected order of
 * stages. For a sharded collection, the 'getAggPlanStages()' function is used to
 * check whether $limit was reordered.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 *   sbe_incompatible,
 * ]
 */
(function() {
load("jstests/libs/analyze_plan.js");  // For getAggPlanStages().

const st = new ShardingTest({shards: 2, config: 1});
const db = st.s.getDB("test");
const coll = db.lookup_with_limit;
const other = db.lookup_with_limit_other;
coll.drop();
other.drop();

// Checks that the order of the pipeline stages matches the expected optimized ordering for an
// unsharded collection.
function checkUnshardedResults(pipeline, expectedPlanStage, expectedPipeline) {
    const explain = coll.explain().aggregate(pipeline);
    assert.eq(explain.stages[0].$cursor.queryPlanner.winningPlan.stage, expectedPlanStage, explain);
    for (let i = 0; i < expectedPipeline.length; i++) {
        assert.eq(Object.keys(explain.stages[i + 1]), expectedPipeline[i], explain);
    }
}

// Checks that the expected stages are pushed down to the query system for a sharded collection.
function checkShardedResults(pipeline, expected) {
    const limitStages = getAggPlanStages(coll.explain().aggregate(pipeline), "LIMIT");
    assert.eq(limitStages.length, expected, limitStages);
}

// Insert ten documents into coll: {x: 0}, {x: 1}, ..., {x: 9}.
const bulk = coll.initializeOrderedBulkOp();
Array.from({length: 10}, (_, i) => ({x: i})).forEach(doc => bulk.insert(doc));
assert.commandWorked(bulk.execute());

// Insert twenty documents into other: {x: 0, y: 0}, {x: 0, y: 1}, ..., {x: 9, y: 0}, {x: 9, y: 1}.
const bulk_other = other.initializeOrderedBulkOp();
Array.from({length: 10}, (_, i) => ({x: i, y: 0})).forEach(doc => bulk_other.insert(doc));
Array.from({length: 10}, (_, i) => ({x: i, y: 1})).forEach(doc => bulk_other.insert(doc));
assert.commandWorked(bulk_other.execute());

// Tests on an unsharded collection.

// Check that lookup->limit is reordered to limit->lookup, with the limit stage pushed down to query
// system.
const lookupPipeline = [
    {$lookup: {from: other.getName(), localField: "x", foreignField: "x", as: "from_other"}},
    {$limit: 5}
];
checkUnshardedResults(lookupPipeline, "LIMIT", ["$lookup"]);

// Check that lookup->addFields->lookup->limit is reordered to limit->lookup->addFields->lookup,
// with the limit stage pushed down to query system.
const multiLookupPipeline = [
    {$lookup: {from: other.getName(), localField: "x", foreignField: "x", as: "from_other"}},
    {$addFields: {z: 0}},
    {$lookup: {from: other.getName(), localField: "x", foreignField: "x", as: "additional"}},
    {$limit: 5}
];
checkUnshardedResults(multiLookupPipeline, "LIMIT", ["$lookup", "$addFields", "$lookup"]);

// Check that lookup->unwind->limit is reordered to lookup->limit, with the unwind stage being
// absorbed into the lookup stage and preventing the limit from swapping before it.
const unwindPipeline = [
    {$lookup: {from: other.getName(), localField: "x", foreignField: "x", as: "from_other"}},
    {$unwind: "$from_other"},
    {$limit: 5}
];
checkUnshardedResults(unwindPipeline, "COLLSCAN", ["$lookup", "$limit"]);

// Check that lookup->unwind->sort->limit is reordered to lookup->sort, with the unwind stage being
// absorbed into the lookup stage and preventing the limit from swapping before it, and the limit
// stage being absorbed into the sort stage.
const sortPipeline = [
    {$lookup: {from: other.getName(), localField: "x", foreignField: "x", as: "from_other"}},
    {$unwind: "$from_other"},
    {$sort: {x: 1}},
    {$limit: 5}
];
checkUnshardedResults(sortPipeline, "COLLSCAN", ["$lookup", "$sort"]);

// Check that sort->lookup->limit is reordered to sort->lookup, with the limit stage being absorbed
// into the sort stage and creating a top-k sort, which is pushed down to query system.
const topKSortPipeline = [
    {$sort: {x: 1}},
    {$lookup: {from: other.getName(), localField: "x", foreignField: "x", as: "from_other"}},
    {$limit: 5}
];
checkUnshardedResults(topKSortPipeline, "SORT", ["$lookup"]);
const explain = coll.explain().aggregate(topKSortPipeline);
assert.eq(explain.stages[0].$cursor.queryPlanner.winningPlan.limitAmount, 5, explain);

// Tests on a sharded collection.
coll.createIndex({x: 1});
st.shardColl(coll, {x: 1}, {x: 1}, {x: 1}, db, true);

checkShardedResults(lookupPipeline, 2);
checkShardedResults(multiLookupPipeline, 2);
checkShardedResults(unwindPipeline, 0);
checkShardedResults(sortPipeline, 0);
checkShardedResults(topKSortPipeline, 2);

st.stop();
}());
