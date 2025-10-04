// Cannot implicitly shard accessed collections because queries on a sharded collection are not
// able to be covered when they aren't on the shard key since the document needs to be fetched in
// order to apply the SHARDING_FILTER stage.
// @tags: [
//   assumes_unsharded_collection,
// ]

// Compound index covered query tests with sort

// Include helpers for analyzing explain output.
import {isIndexOnly} from "jstests/libs/query/analyze_plan.js";

let coll = db.getCollection("covered_sort_3");
coll.drop();
for (let i = 0; i < 100; i++) {
    coll.insert({a: i, b: "strvar_" + (i % 13), c: NumberInt(i % 10)});
}

coll.createIndex({a: 1, b: -1, c: 1});

// Test no query, sort on all fields in index order
let plan = coll
    .find({}, {b: 1, c: 1, _id: 0})
    .sort({a: 1, b: -1, c: 1})
    .hint({a: 1, b: -1, c: 1})
    .explain("executionStats");
assert(isIndexOnly(db, plan.queryPlanner.winningPlan), "sort.3.1 - indexOnly should be true on covered query");
assert.eq(0, plan.executionStats.totalDocsExamined, "sort.3.1 - docs examined should be 0 for covered query");

print("all tests pass");
