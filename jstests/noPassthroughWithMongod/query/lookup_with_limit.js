/**
 * Tests that the $limit stage is pushed before $lookup stages, except when there is an $unwind.
 */
import {flattenQueryPlanTree, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {
    checkSbeFullFeatureFlagEnabled,
    checkSbeFullyEnabled,
    checkSbeRestrictedOrFullyEnabled
} from "jstests/libs/query/sbe_util.js";

const isFeatureFlagSbeFullEnabled = checkSbeFullFeatureFlagEnabled(db);
const isSbeEnabled = checkSbeFullyEnabled(db);
const isSbeGroupLookupOnly = checkSbeRestrictedOrFullyEnabled(db);

const coll = db.lookup_with_limit;
const other = db.lookup_with_limit_other;
coll.drop();
other.drop();

// Checks that the order of the query stages and pipeline stages matches the expected ordering
// depending on whether the pipeline is optimized or not.
function checkResults(pipeline, isOptimized, expected) {
    assert.commandWorked(db.adminCommand({
        "configureFailPoint": 'disablePipelineOptimization',
        "mode": isOptimized ? 'off' : 'alwaysOn'
    }));
    const explain = coll.explain().aggregate(pipeline);
    if (explain.stages) {
        const queryStages =
            flattenQueryPlanTree(getWinningPlanFromExplain(explain.stages[0].$cursor.queryPlanner));
        const pipelineStages = explain.stages.slice(1).map(s => Object.keys(s)[0]);
        assert.eq(queryStages.concat(pipelineStages), expected, explain);
    } else {
        const queryStages = flattenQueryPlanTree(getWinningPlanFromExplain(explain.queryPlanner));
        assert.eq(queryStages, expected, explain);
    }
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

////////////////////////////////////////////////////////////////////////////////////////////////////
// TESTS
////////////////////////////////////////////////////////////////////////////////////////////////////

// TEST_01: Check that lookup->limit is reordered to limit->lookup, with the limit stage pushed down
// to query system.
let pipeline = [
    {$lookup: {from: other.getName(), localField: "x", foreignField: "x", as: "from_other"}},
    {$limit: 5}
];
if (isSbeEnabled) {
    checkResults(pipeline, false, ["COLLSCAN", "EQ_LOOKUP", "LIMIT"]);
    checkResults(pipeline, true, ["COLLSCAN", "LIMIT", "EQ_LOOKUP"]);
} else if (isSbeGroupLookupOnly) {
    checkResults(pipeline, false, ["COLLSCAN", "EQ_LOOKUP", "$limit"]);
    checkResults(pipeline, true, ["COLLSCAN", "LIMIT", "EQ_LOOKUP"]);
} else {
    checkResults(pipeline, false, ["COLLSCAN", "$lookup", "$limit"]);
    checkResults(pipeline, true, ["COLLSCAN", "LIMIT", "$lookup"]);
}

// TEST_02: Check that lookup->addFields->lookup->limit is reordered to
// limit->lookup->addFields->lookup, with the limit stage pushed down to query system.
pipeline = [
    {$lookup: {from: other.getName(), localField: "x", foreignField: "x", as: "from_other"}},
    {$addFields: {z: 0}},
    {$lookup: {from: other.getName(), localField: "x", foreignField: "x", as: "additional"}},
    {$limit: 5}
];
if (isSbeEnabled) {
    checkResults(
        pipeline, false, ["COLLSCAN", "EQ_LOOKUP", "PROJECTION_DEFAULT", "EQ_LOOKUP", "LIMIT"]);
    checkResults(
        pipeline, true, ["COLLSCAN", "LIMIT", "EQ_LOOKUP", "PROJECTION_DEFAULT", "EQ_LOOKUP"]);
} else if (isSbeGroupLookupOnly) {
    checkResults(pipeline, false, ["COLLSCAN", "EQ_LOOKUP", "$addFields", "$lookup", "$limit"]);
    checkResults(pipeline, true, ["COLLSCAN", "LIMIT", "EQ_LOOKUP", "$addFields", "$lookup"]);
} else {
    checkResults(pipeline, false, ["COLLSCAN", "$lookup", "$addFields", "$lookup", "$limit"]);
    checkResults(pipeline, true, ["COLLSCAN", "LIMIT", "$lookup", "$addFields", "$lookup"]);
}

// TEST_03: Check that lookup->unwind->limit is reordered to lookup->limit, with the unwind stage
// being absorbed into the lookup stage and preventing the limit from swapping before it.
pipeline = [
    {$lookup: {from: other.getName(), localField: "x", foreignField: "x", as: "from_other"}},
    {$unwind: "$from_other"},
    {$limit: 5}
];
if (isFeatureFlagSbeFullEnabled) {
    checkResults(pipeline, false, ["COLLSCAN", "EQ_LOOKUP", "UNWIND", "LIMIT"]);
    checkResults(pipeline, true, ["COLLSCAN", "EQ_LOOKUP_UNWIND", "LIMIT"]);
} else if (isSbeEnabled) {
    checkResults(pipeline, false, ["COLLSCAN", "EQ_LOOKUP", "$unwind", "$limit"]);
    checkResults(pipeline, true, ["COLLSCAN", "EQ_LOOKUP_UNWIND", "LIMIT"]);
} else if (isSbeGroupLookupOnly) {
    checkResults(pipeline, false, ["COLLSCAN", "EQ_LOOKUP", "$unwind", "$limit"]);
    checkResults(pipeline, true, ["COLLSCAN", "$lookup", "$limit"]);
} else {
    checkResults(pipeline, false, ["COLLSCAN", "$lookup", "$unwind", "$limit"]);
    checkResults(pipeline, true, ["COLLSCAN", "$lookup", "$limit"]);
}

// TEST_04: Same as TEST_03 except intervening $unset prevents the $unwind from being absorbed into
// the $lookup.
pipeline = [
    {$lookup: {from: other.getName(), localField: "x", foreignField: "x", as: "from_other"}},
    {$unset: "nonexistent_field"},
    {$unwind: "$from_other"},
    {$limit: 5}
];
if (isFeatureFlagSbeFullEnabled) {
    checkResults(
        pipeline, false, ["COLLSCAN", "EQ_LOOKUP", "PROJECTION_DEFAULT", "UNWIND", "LIMIT"]);
    checkResults(
        pipeline, true, ["COLLSCAN", "EQ_LOOKUP", "PROJECTION_DEFAULT", "UNWIND", "LIMIT"]);
} else if (isSbeEnabled) {
    checkResults(
        pipeline, false, ["COLLSCAN", "EQ_LOOKUP", "PROJECTION_DEFAULT", "$unwind", "$limit"]);
    checkResults(
        pipeline, true, ["COLLSCAN", "EQ_LOOKUP", "PROJECTION_DEFAULT", "$unwind", "$limit"]);
} else if (isSbeGroupLookupOnly) {
    checkResults(pipeline, false, ["COLLSCAN", "EQ_LOOKUP", "$project", "$unwind", "$limit"]);
    checkResults(pipeline, true, ["COLLSCAN", "EQ_LOOKUP", "$project", "$unwind", "$limit"]);
} else {
    checkResults(pipeline, false, ["COLLSCAN", "$lookup", "$project", "$unwind", "$limit"]);
    checkResults(pipeline, true, ["COLLSCAN", "$lookup", "$project", "$unwind", "$limit"]);
}

// TEST_05: Check that lookup->unwind->sort->limit is reordered to lookup->sort, with the unwind
// stage being absorbed into the lookup stage and preventing the limit from swapping before it, and
// the limit stage being absorbed into the sort stage.
pipeline = [
    {$lookup: {from: other.getName(), localField: "x", foreignField: "x", as: "from_other"}},
    {$unwind: "$from_other"},
    {$sort: {x: 1}},
    {$limit: 5}
];

if (isFeatureFlagSbeFullEnabled) {
    checkResults(pipeline, false, ["COLLSCAN", "EQ_LOOKUP", "UNWIND", "SORT", "LIMIT"]);
    checkResults(pipeline, true, ["COLLSCAN", "EQ_LOOKUP_UNWIND", "SORT"]);
} else if (isSbeEnabled) {
    checkResults(pipeline, false, ["COLLSCAN", "EQ_LOOKUP", "$unwind", "$sort", "$limit"]);
    checkResults(pipeline, true, ["COLLSCAN", "EQ_LOOKUP_UNWIND", "SORT"]);
} else if (isSbeGroupLookupOnly) {
    checkResults(pipeline, false, ["COLLSCAN", "EQ_LOOKUP", "$unwind", "$sort", "$limit"]);
    checkResults(pipeline, true, ["COLLSCAN", "$lookup", "$sort"]);
} else {
    checkResults(pipeline, false, ["COLLSCAN", "$lookup", "$unwind", "$sort", "$limit"]);
    checkResults(pipeline, true, ["COLLSCAN", "$lookup", "$sort"]);
}

// TEST_06: Same as TEST_05 except intervening $unset prevents the $unwind from being absorbed into
// the $lookup. The $sort is still moved before the $unwind in the optimized case.
pipeline = [
    {$lookup: {from: other.getName(), localField: "x", foreignField: "x", as: "from_other"}},
    {$unset: "nonexistent_field"},
    {$unwind: "$from_other"},
    {$sort: {x: 1}},
    {$limit: 5}
];
if (isFeatureFlagSbeFullEnabled) {
    checkResults(pipeline,
                 false,
                 ["COLLSCAN", "EQ_LOOKUP", "PROJECTION_DEFAULT", "UNWIND", "SORT", "LIMIT"]);
    checkResults(
        pipeline, true, ["COLLSCAN", "EQ_LOOKUP", "PROJECTION_DEFAULT", "SORT", "UNWIND", "LIMIT"]);
} else if (isSbeEnabled) {
    checkResults(pipeline,
                 false,
                 ["COLLSCAN", "EQ_LOOKUP", "PROJECTION_DEFAULT", "$unwind", "$sort", "$limit"]);
    checkResults(pipeline,
                 true,
                 ["COLLSCAN", "EQ_LOOKUP", "PROJECTION_DEFAULT", "SORT", "$unwind", "$limit"]);
} else if (isSbeGroupLookupOnly) {
    checkResults(
        pipeline, false, ["COLLSCAN", "EQ_LOOKUP", "$project", "$unwind", "$sort", "$limit"]);
    checkResults(
        pipeline, true, ["COLLSCAN", "EQ_LOOKUP", "$project", "$sort", "$unwind", "$limit"]);
} else {
    checkResults(
        pipeline, false, ["COLLSCAN", "$lookup", "$project", "$unwind", "$sort", "$limit"]);
    checkResults(pipeline, true, ["COLLSCAN", "$lookup", "$project", "$sort", "$unwind", "$limit"]);
}
