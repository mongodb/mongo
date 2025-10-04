// Tests the behavior of explain() when used with aggregation pipeline to
// verify cursorType field. This test verifies that the cursorType field
// is correctly exposed in the $cursor.queryPlanner stage explain output.
//
// @tags: [
//   assumes_unsharded_collection,
//   do_not_wrap_aggregations_in_facets,
// ]
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";

if (checkSbeRestrictedOrFullyEnabled(db)) {
    jsTest.log.info("Skipping test because $count queries don't use the emptyDocuments cursor in SBE");
    quit();
}

const coll = db.explain_cursor;
coll.drop();

const kNumDocs = 10;

for (let i = 0; i < kNumDocs; i++) {
    assert.commandWorked(coll.insert({_id: i, a: i, b: i % 2}));
}

function getCursorType(explainOutput) {
    const cursorStage = getAggPlanStage(explainOutput, "$cursor");
    assert.neq(null, cursorStage, "No $cursor stage present");
    assert(cursorStage.$cursor.hasOwnProperty("queryPlanner"), "No $cursor.queryPlanner present");
    assert(cursorStage.$cursor.queryPlanner.hasOwnProperty("cursorType"), "No $cursor.queryPlanner.cursorType present");
    return cursorStage.$cursor.queryPlanner.cursorType;
}

// Normal aggregation queries should use the regular cursor.
const groupExplain = coll.explain().aggregate([{$match: {b: 1}}, {$group: {_id: "$a", count: {$sum: 1}}}]);
assert.eq("regular", getCursorType(groupExplain));

const multiStageExplain = coll
    .explain()
    .aggregate([
        {$match: {a: {$lt: 8}}},
        {$project: {a: 1, category: {$cond: [{$gt: ["$a", 5]}, "high", "low"]}}},
        {$sort: {category: 1, a: -1}},
    ]);
assert.eq("regular", getCursorType(multiStageExplain));

// Count-like queries should use the emptyDocuments cursor.
const countExplain = coll.explain().aggregate([{$match: {a: {$gte: 5}}}, {$count: "filtered"}]);
assert.eq("emptyDocuments", getCursorType(countExplain));

const multiStageCountExplain = coll.explain().aggregate([
    {
        "$addFields": {
            "c": NumberInt(0),
        },
    },
    {
        "$project": {
            "_id": 0,
            "c": 1,
        },
    },
]);
assert.eq("emptyDocuments", getCursorType(multiStageCountExplain));
