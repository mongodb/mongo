// Simple covered index query test with a unique sparse index

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");

var coll = db.getCollection("covered_simple_3");
coll.drop();
for (i = 0; i < 10; i++) {
    coll.insert({foo: i});
}
for (i = 0; i < 5; i++) {
    coll.insert({bar: i});
}
coll.insert({foo: "string"});
coll.insert({foo: {bar: 1}});
coll.insert({foo: null});
coll.ensureIndex({foo: 1}, {sparse: true, unique: true});

// Test equality with int value
var plan = coll.find({foo: 1}, {foo: 1, _id: 0}).hint({foo: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "simple.3.1 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.3.1 - docs examined should be 0 for covered query");

// Test equality with string value
var plan = coll.find({foo: "string"}, {foo: 1, _id: 0}).hint({foo: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "simple.3.2 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.3.2 - docs examined should be 0 for covered query");

// Test equality with int value on a dotted field
var plan = coll.find({foo: {bar: 1}}, {foo: 1, _id: 0}).hint({foo: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "simple.3.3 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.3.3 - docs examined should be 0 for covered query");

// Test no query
var plan = coll.find({}, {foo: 1, _id: 0}).hint({foo: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "simple.3.4 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.3.4 - docs examined should be 0 for covered query");

// Test range query
var plan =
    coll.find({foo: {$gt: 2, $lt: 6}}, {foo: 1, _id: 0}).hint({foo: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "simple.3.5 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.3.5 - docs examined should be 0 for covered query");

// Test in query
var plan =
    coll.find({foo: {$in: [5, 8]}}, {foo: 1, _id: 0}).hint({foo: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "simple.3.6 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.3.6 - docs examined should be 0 for covered query");

// Test $exists true
var plan =
    coll.find({foo: {$exists: true}}, {foo: 1, _id: 0}).hint({foo: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "simple.3.7 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.3.7 - docs examined should be 0 for covered query");

// Check that $nin can be covered.
coll.dropIndexes();
coll.ensureIndex({bar: 1});
var plan =
    coll.find({bar: {$nin: [5, 8]}}, {bar: 1, _id: 0}).hint({bar: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "simple.3.8 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.3.8 - docs examined should be 0 for covered query");

print('all tests pass');
