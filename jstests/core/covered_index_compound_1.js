// Compound index covered query tests

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");

var coll = db.getCollection("covered_compound_1");
coll.drop();
for (i = 0; i < 100; i++) {
    coll.insert({a: i, b: "strvar_" + (i % 13), c: NumberInt(i % 10)});
}
coll.ensureIndex({a: 1, b: -1, c: 1});

// Test equality - all indexed fields queried and projected
var plan = coll.find({a: 10, b: "strvar_10", c: 0}, {a: 1, b: 1, c: 1, _id: 0})
               .hint({a: 1, b: -1, c: 1})
               .explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "compound.1.1 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "compound.1.1 - nscannedObjects should be 0 for covered query");

// Test query on subset of fields queried and project all
var plan = coll.find({a: 26, b: "strvar_0"}, {a: 1, b: 1, c: 1, _id: 0})
               .hint({a: 1, b: -1, c: 1})
               .explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "compound.1.2 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "compound.1.2 - nscannedObjects should be 0 for covered query");

// Test query on all fields queried and project subset
var plan = coll.find({a: 38, b: "strvar_12", c: 8}, {b: 1, c: 1, _id: 0})
               .hint({a: 1, b: -1, c: 1})
               .explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "compound.1.3 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "compound.1.3 - nscannedObjects should be 0 for covered query");

// Test no query
var plan = coll.find({}, {b: 1, c: 1, _id: 0}).hint({a: 1, b: -1, c: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "compound.1.4 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "compound.1.4 - nscannedObjects should be 0 for covered query");

// Test range query
var plan = coll.find({a: {$gt: 25, $lt: 43}}, {b: 1, c: 1, _id: 0})
               .hint({a: 1, b: -1, c: 1})
               .explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "compound.1.5 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "compound.1.5 - nscannedObjects should be 0 for covered query");

// Test in query
var plan = coll.find({a: 38, b: "strvar_12", c: {$in: [5, 8]}}, {b: 1, c: 1, _id: 0})
               .hint({a: 1, b: -1, c: 1})
               .explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "compound.1.6 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "compound.1.6 - nscannedObjects should be 0 for covered query");

// Test no result
var plan = coll.find({a: 38, b: "strvar_12", c: 55}, {a: 1, b: 1, c: 1, _id: 0})
               .hint({a: 1, b: -1, c: 1})
               .explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "compound.1.7 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "compound.1.7 - nscannedObjects should be 0 for covered query");

print('all tests passed');
