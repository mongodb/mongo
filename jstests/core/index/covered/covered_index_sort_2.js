// Simple covered index query test with sort on _id
// @tags: [
//   assumes_balancer_off,
//   assumes_read_concern_local,
// ]

// Include helpers for analyzing explain output.
import {isIndexOnly} from "jstests/libs/query/analyze_plan.js";

let coll = db.getCollection("covered_sort_2");
coll.drop();
for (let i = 0; i < 10; i++) {
    coll.insert({_id: i});
}
coll.insert({_id: "1"});
coll.insert({_id: {bar: 1}});
coll.insert({_id: null});

// Test no query
let plan = coll.find({}, {_id: 1}).sort({_id: -1}).hint({_id: 1}).explain("executionStats");
assert(isIndexOnly(db, plan.queryPlanner.winningPlan), "sort.2.1 - indexOnly should be true on covered query");
assert.eq(0, plan.executionStats.totalDocsExamined, "sort.2.1 - docs examined should be 0 for covered query");

print("all tests pass");
