// Simple covered index query test with sort on _id

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");

var coll = db.getCollection("covered_sort_2");
coll.drop();
for (i = 0; i < 10; i++) {
    coll.insert({_id: i});
}
coll.insert({_id: "1"});
coll.insert({_id: {bar: 1}});
coll.insert({_id: null});

// Test no query
var plan = coll.find({}, {_id: 1}).sort({_id: -1}).hint({_id: 1}).explain("executionStats");
assert(isIndexOnly(plan.queryPlanner.winningPlan),
       "sort.2.1 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "sort.2.1 - docs examined should be 0 for covered query");

print('all tests pass');
