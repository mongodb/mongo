// Simple covered index query test with sort

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");

var coll = db.getCollection("covered_sort_1");
coll.drop();
for (i = 0; i < 10; i++) {
    coll.insert({foo: i});
}
for (i = 0; i < 10; i++) {
    coll.insert({foo: i});
}
for (i = 0; i < 5; i++) {
    coll.insert({bar: i});
}
coll.insert({foo: "1"});
coll.insert({foo: {bar: 1}});
coll.insert({foo: null});
coll.ensureIndex({foo: 1});

// Test no query and sort ascending
var plan = coll.find({}, {foo: 1, _id: 0}).sort({foo: 1}).hint({foo: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "sort.1.1 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "sort.1.1 - docs examined should be 0 for covered query");

// Test no query and sort descending
var plan = coll.find({}, {foo: 1, _id: 0}).sort({foo: -1}).hint({foo: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "sort.1.2 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "sort.1.2 - docs examined should be 0 for covered query");

// Test range query with sort
var plan = coll.find({foo: {$gt: 2}}, {foo: 1, _id: 0})
               .sort({foo: -1})
               .hint({foo: 1})
               .explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "sort.1.3 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "sort.1.3 - docs examined should be 0 for covered query");

print('all tests pass');
