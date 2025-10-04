// Cannot implicitly shard accessed collections because queries on a sharded collection are not
// able to be covered when they aren't on the shard key since the document needs to be fetched in
// order to apply the SHARDING_FILTER stage.
// @tags: [
//   assumes_unsharded_collection,
// ]

// Simple covered index query test with sort

// Include helpers for analyzing explain output.
import {isIndexOnly} from "jstests/libs/query/analyze_plan.js";

let coll = db.getCollection("covered_sort_1");
coll.drop();
for (let i = 0; i < 10; i++) {
    coll.insert({foo: i});
}
for (let i = 0; i < 10; i++) {
    coll.insert({foo: i});
}
for (let i = 0; i < 5; i++) {
    coll.insert({bar: i});
}
coll.insert({foo: "1"});
coll.insert({foo: {bar: 1}});
coll.insert({foo: null});
coll.createIndex({foo: 1});

// Test no query and sort ascending
var plan = coll.find({}, {foo: 1, _id: 0}).sort({foo: 1}).hint({foo: 1}).explain("executionStats");
assert(isIndexOnly(db, plan.queryPlanner.winningPlan), "sort.1.1 - indexOnly should be true on covered query");
assert.eq(0, plan.executionStats.totalDocsExamined, "sort.1.1 - docs examined should be 0 for covered query");

// Test no query and sort descending
var plan = coll.find({}, {foo: 1, _id: 0}).sort({foo: -1}).hint({foo: 1}).explain("executionStats");
assert(isIndexOnly(db, plan.queryPlanner.winningPlan), "sort.1.2 - indexOnly should be true on covered query");
assert.eq(0, plan.executionStats.totalDocsExamined, "sort.1.2 - docs examined should be 0 for covered query");

// Test range query with sort
var plan = coll
    .find({foo: {$gt: 2}}, {foo: 1, _id: 0})
    .sort({foo: -1})
    .hint({foo: 1})
    .explain("executionStats");
assert(isIndexOnly(db, plan.queryPlanner.winningPlan), "sort.1.3 - indexOnly should be true on covered query");
assert.eq(0, plan.executionStats.totalDocsExamined, "sort.1.3 - docs examined should be 0 for covered query");

print("all tests pass");
