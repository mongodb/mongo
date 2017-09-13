// Simple covered index query test

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");

var coll = db.getCollection("covered_simple_id");
coll.drop();
for (i = 0; i < 10; i++) {
    coll.insert({_id: i});
}
coll.insert({_id: "string"});
coll.insert({_id: {bar: 1}});
coll.insert({_id: null});

// Test equality with int value
var plan = coll.find({_id: 1}, {_id: 1}).hint({_id: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "simple.id.1 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.id.1 - docs examined should be 0 for covered query");

// Test equality with string value
var plan = coll.find({_id: "string"}, {_id: 1}).hint({_id: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "simple.id.2 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.id.2 - docs examined should be 0 for covered query");

// Test equality with int value on a dotted field
var plan = coll.find({_id: {bar: 1}}, {_id: 1}).hint({_id: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "simple.id.3 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.id.3 - docs examined should be 0 for covered query");

// Test no query
var plan = coll.find({}, {_id: 1}).hint({_id: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "simple.id.4 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.id.4 - docs examined should be 0 for covered query");

// Test range query
var plan = coll.find({_id: {$gt: 2, $lt: 6}}, {_id: 1}).hint({_id: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "simple.id.5 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.id.5 - docs examined should be 0 for covered query");

// Test in query
var plan = coll.find({_id: {$in: [5, 8]}}, {_id: 1}).hint({_id: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "simple.id.6 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.id.6 - docs examined should be 0 for covered query");

print('all tests pass');
